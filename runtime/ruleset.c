/* ruleset.c - rsyslog's ruleset object
 *
 * We have a two-way structure of linked lists: one config-specifc linked list
 * (conf->rulesets.llRulesets) hold alls rule sets that we know. Included in each
 * list is a list of rules (which contain a list of actions, but that's
 * a different story).
 *
 * Usually, only a single rule set is executed. However, there exist some
 * situations where all rules must be iterated over, for example on HUP. Thus,
 * we also provide interfaces to do that.
 *
 * Module begun 2009-06-10 by Rainer Gerhards
 *
 * Copyright 2009-2012 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of the rsyslog runtime library.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>

#include "rsyslog.h"
#include "obj.h"
#include "cfsysline.h"
#include "msg.h"
#include "ruleset.h"
#include "rule.h"
#include "errmsg.h"
#include "parser.h"
#include "batch.h"
#include "unicode-helper.h"
#include "rsconf.h"
#include "dirty.h" /* for main ruleset queue creation */

/* static data */
DEFobjStaticHelpers
DEFobjCurrIf(errmsg)
DEFobjCurrIf(rule)
DEFobjCurrIf(parser)

/* forward definitions */
static rsRetVal processBatch(batch_t *pBatch);


/* ---------- linked-list key handling functions (ruleset) ---------- */

/* destructor for linked list keys.
 */
rsRetVal
rulesetKeyDestruct(void __attribute__((unused)) *pData)
{
	free(pData);
	return RS_RET_OK;
}
/* ---------- END linked-list key handling functions (ruleset) ---------- */



/* driver to iterate over all of this ruleset actions */
typedef struct iterateAllActions_s {
	rsRetVal (*pFunc)(void*, void*);
	void *pParam;
} iterateAllActions_t;
DEFFUNC_llExecFunc(doIterateRulesetActions)
{
	DEFiRet;
	rule_t* pRule = (rule_t*) pData;
	iterateAllActions_t *pMyParam = (iterateAllActions_t*) pParam;
	iRet = rule.IterateAllActions(pRule, pMyParam->pFunc, pMyParam->pParam);
	RETiRet;
}
/* iterate over all actions of THIS rule set.
 */
static rsRetVal
iterateRulesetAllActions(ruleset_t *pThis, rsRetVal (*pFunc)(void*, void*), void* pParam)
{
	iterateAllActions_t params;
	DEFiRet;
	assert(pFunc != NULL);

	params.pFunc = pFunc;
	params.pParam = pParam;
	CHKiRet(llExecFunc(&(pThis->llRules), doIterateRulesetActions, &params));

finalize_it:
	RETiRet;
}


/* driver to iterate over all actions */
DEFFUNC_llExecFunc(doIterateAllActions)
{
	DEFiRet;
	ruleset_t* pThis = (ruleset_t*) pData;
	iterateAllActions_t *pMyParam = (iterateAllActions_t*) pParam;
	iRet = iterateRulesetAllActions(pThis, pMyParam->pFunc, pMyParam->pParam);
	RETiRet;
}
/* iterate over ALL actions present in the WHOLE system.
 * this is often needed, for example when HUP processing 
 * must be done or a shutdown is pending.
 */
static rsRetVal
iterateAllActions(rsconf_t *conf, rsRetVal (*pFunc)(void*, void*), void* pParam)
{
	iterateAllActions_t params;
	DEFiRet;
	assert(pFunc != NULL);

	params.pFunc = pFunc;
	params.pParam = pParam;
	CHKiRet(llExecFunc(&(conf->rulesets.llRulesets), doIterateAllActions, &params));

finalize_it:
	RETiRet;
}



/* helper to processBatch(), used to call the configured actions. It is
 * executed from within llExecFunc() of the action list.
 * rgerhards, 2007-08-02
 */
DEFFUNC_llExecFunc(processBatchDoRules)
{
	rsRetVal iRet;
	ISOBJ_TYPE_assert(pData, rule);
	DBGPRINTF("Processing next rule\n");
	iRet = rule.ProcessBatch((rule_t*) pData, (batch_t*) pParam);
	DBGPRINTF("ruleset: get iRet %d from rule.ProcessMsg()\n", iRet);
	return iRet;
}



/* This function is similar to processBatch(), but works on a batch that
 * contains rules from multiple rulesets. In this case, we can not push
 * the whole batch through the ruleset. Instead, we examine it and
 * partition it into sub-rulesets which we then push through the system.
 * Note that when we evaluate which message must be processed, we do NOT need
 * to look at bFilterOK, because this value is only set in a later processing
 * stage. Doing so caused a bug during development ;)
 * rgerhards, 2010-06-15
 */
static inline rsRetVal
processBatchMultiRuleset(batch_t *pBatch)
{
	ruleset_t *currRuleset;
	batch_t snglRuleBatch;
	int i;
	int iStart;	/* start index of partial batch */
	int iNew;	/* index for new (temporary) batch */
	int bHaveUnprocessed;	/* do we (still) have unprocessed entries? (loop term predicate) */
	DEFiRet;

	do {
		bHaveUnprocessed = 0;
		/* search for first unprocessed element */
		for(iStart = 0 ; iStart < pBatch->nElem && pBatch->pElem[iStart].state == BATCH_STATE_DISC ; ++iStart)
			/* just search, no action */;
		if(iStart == pBatch->nElem)
			break; /* everything processed */

		/* prepare temporary batch */
		CHKiRet(batchInit(&snglRuleBatch, pBatch->nElem));
		snglRuleBatch.pbShutdownImmediate = pBatch->pbShutdownImmediate;
		currRuleset = batchElemGetRuleset(pBatch, iStart);
		iNew = 0;
		for(i = iStart ; i < pBatch->nElem ; ++i) {
			if(batchElemGetRuleset(pBatch, i) == currRuleset) {
				/* for performance reasons, we copy only those members that we actually need */
				snglRuleBatch.pElem[iNew].pUsrp = pBatch->pElem[i].pUsrp;
				snglRuleBatch.pElem[iNew].state = pBatch->pElem[i].state;
				++iNew;
				/* We indicate the element also as done, so it will not be processed again */
				pBatch->pElem[i].state = BATCH_STATE_DISC;
			} else {
				bHaveUnprocessed = 1;
			}
		}
		snglRuleBatch.nElem = iNew; /* was left just right by the for loop */
		batchSetSingleRuleset(&snglRuleBatch, 1);
		/* process temp batch */
		processBatch(&snglRuleBatch);
		batchFree(&snglRuleBatch);
	} while(bHaveUnprocessed == 1);

finalize_it:
	RETiRet;
}

/* Process (consume) a batch of messages. Calls the actions configured.
 * If the whole batch uses a singel ruleset, we can process the batch as 
 * a whole. Otherwise, we need to process it slower, on a message-by-message
 * basis (what can be optimized to a per-ruleset basis)
 * rgerhards, 2005-10-13
 */
static rsRetVal
processBatch(batch_t *pBatch)
{
	ruleset_t *pThis;
	DEFiRet;
	assert(pBatch != NULL);

	DBGPRINTF("processBatch: batch of %d elements must be processed\n", pBatch->nElem);
	if(pBatch->bSingleRuleset) {
		pThis = batchGetRuleset(pBatch);
		if(pThis == NULL)
			pThis = ourConf->rulesets.pDflt;
		ISOBJ_TYPE_assert(pThis, ruleset);
		CHKiRet(llExecFunc(&pThis->llRules, processBatchDoRules, pBatch));
	} else {
		CHKiRet(processBatchMultiRuleset(pBatch));
	}

finalize_it:
	DBGPRINTF("ruleset.ProcessMsg() returns %d\n", iRet);
	RETiRet;
}


/* return the ruleset-assigned parser list. NULL means use the default
 * parser list.
 * rgerhards, 2009-11-04
 */
static parserList_t*
GetParserList(rsconf_t *conf, msg_t *pMsg)
{
	return (pMsg->pRuleset == NULL) ? conf->rulesets.pDflt->pParserLst : pMsg->pRuleset->pParserLst;
}


/* Add a script block to the current ruleset */
static void
addScript(ruleset_t *pThis, struct cnfstmt *script)
{
	if(pThis->last == NULL)
		pThis->root = pThis->last = script;
	else {
		pThis->last->next = script;
		pThis->last = script;
	}
dbgprintf("RRRR: ruleset added script, script total now is:\n");
	cnfstmtPrint(pThis->root, 0);
}

/* Add a new rule to the end of the current rule set. We do a number
 * of checks and ignore the rule if it does not pass them.
 */
static rsRetVal
addRule(ruleset_t *pThis, rule_t **ppRule)
{
	int iActionCnt;
	DEFiRet;

	ISOBJ_TYPE_assert(pThis, ruleset);
	ISOBJ_TYPE_assert(*ppRule, rule);

	CHKiRet(llGetNumElts(&(*ppRule)->llActList, &iActionCnt));
	if(iActionCnt == 0) {
		errmsg.LogError(0, NO_ERRCODE, "warning: selector line without actions will be discarded");
		rule.Destruct(ppRule);
	} else {
		CHKiRet(llAppend(&pThis->llRules, NULL, *ppRule));
		DBGPRINTF("selector line successfully processed, %d actions\n", iActionCnt);
	}

finalize_it:
	RETiRet;
}


/* set name for ruleset */
static rsRetVal setName(ruleset_t *pThis, uchar *pszName)
{
	DEFiRet;
	free(pThis->pszName);
	CHKmalloc(pThis->pszName = ustrdup(pszName));

finalize_it:
	RETiRet;
}


/* get current ruleset
 * We use a non-standard calling interface, as nothing can go wrong and it
 * is really much more natural to return the pointer directly.
 */
static ruleset_t*
GetCurrent(rsconf_t *conf)
{
	return conf->rulesets.pCurr;
}


/* get main queue associated with ruleset. If no ruleset-specifc main queue
 * is set, the primary main message queue is returned.
 * We use a non-standard calling interface, as nothing can go wrong and it
 * is really much more natural to return the pointer directly.
 */
static qqueue_t*
GetRulesetQueue(ruleset_t *pThis)
{
	ISOBJ_TYPE_assert(pThis, ruleset);
	return (pThis->pQueue == NULL) ? pMsgQueue : pThis->pQueue;
}


/* Find the ruleset with the given name and return a pointer to its object.
 */
rsRetVal
rulesetGetRuleset(rsconf_t *conf, ruleset_t **ppRuleset, uchar *pszName)
{
	DEFiRet;
	assert(ppRuleset != NULL);
	assert(pszName != NULL);

	CHKiRet(llFind(&(conf->rulesets.llRulesets), pszName, (void*) ppRuleset));

finalize_it:
	RETiRet;
}


/* Set a new default rule set. If the default can not be found, no change happens.
 */
static rsRetVal
SetDefaultRuleset(rsconf_t *conf, uchar *pszName)
{
	ruleset_t *pRuleset;
	DEFiRet;
	assert(pszName != NULL);

	CHKiRet(rulesetGetRuleset(conf, &pRuleset, pszName));
	conf->rulesets.pDflt = pRuleset;
	DBGPRINTF("default rule set changed to %p: '%s'\n", pRuleset, pszName);

finalize_it:
	RETiRet;
}


/* Set a new current rule set. If the ruleset can not be found, no change happens.
 */
static rsRetVal
SetCurrRuleset(rsconf_t *conf, uchar *pszName)
{
	ruleset_t *pRuleset;
	DEFiRet;
	assert(pszName != NULL);

	CHKiRet(rulesetGetRuleset(conf, &pRuleset, pszName));
	conf->rulesets.pCurr = pRuleset;
	DBGPRINTF("current rule set changed to %p: '%s'\n", pRuleset, pszName);

finalize_it:
	RETiRet;
}


/* destructor we need to destruct rules inside our linked list contents.
 */
static rsRetVal
doRuleDestruct(void *pData)
{
	rule_t *pRule = (rule_t *) pData;
	DEFiRet;
	rule.Destruct(&pRule);
	RETiRet;
}


/* Standard-Constructor
 */
BEGINobjConstruct(ruleset) /* be sure to specify the object type also in END macro! */
	CHKiRet(llInit(&pThis->llRules, doRuleDestruct, NULL, NULL));
	pThis->root = NULL;
	pThis->last = NULL;
finalize_it:
ENDobjConstruct(ruleset)


/* ConstructionFinalizer
 * This also adds the rule set to the list of all known rulesets.
 */
static rsRetVal
rulesetConstructFinalize(rsconf_t *conf, ruleset_t *pThis)
{
	uchar *keyName;
	DEFiRet;
	ISOBJ_TYPE_assert(pThis, ruleset);

	/* we must duplicate our name, as the key destructer would also
	 * free it, resulting in a double-free. It's also cleaner to have
	 * two separate copies.
	 */
	CHKmalloc(keyName = ustrdup(pThis->pszName));
	CHKiRet(llAppend(&(conf->rulesets.llRulesets), keyName, pThis));

	/* this now also is the new current ruleset */
	conf->rulesets.pCurr = pThis;

	/* and also the default, if so far none has been set */
	if(conf->rulesets.pDflt == NULL)
		conf->rulesets.pDflt = pThis;

finalize_it:
	RETiRet;
}


/* destructor for the ruleset object */
BEGINobjDestruct(ruleset) /* be sure to specify the object type also in END and CODESTART macros! */
CODESTARTobjDestruct(ruleset)
	DBGPRINTF("destructing ruleset %p, name %p\n", pThis, pThis->pszName);
	if(pThis->pQueue != NULL) {
		qqueueDestruct(&pThis->pQueue);
	}
	if(pThis->pParserLst != NULL) {
		parser.DestructParserList(&pThis->pParserLst);
	}
	llDestroy(&pThis->llRules);
	free(pThis->pszName);
	// TODO: free rainerscript root (not look at last)
ENDobjDestruct(ruleset)


/* destruct ALL rule sets that reside in the system. This must
 * be callable before unloading this module as the module may
 * not be unloaded before unload of the actions is required. This is
 * kind of a left-over from previous logic and may be optimized one
 * everything runs stable again. -- rgerhards, 2009-06-10
 */
static rsRetVal
destructAllActions(rsconf_t *conf)
{
	DEFiRet;

	CHKiRet(llDestroy(&(conf->rulesets.llRulesets)));
	CHKiRet(llInit(&(conf->rulesets.llRulesets), rulesetDestructForLinkedList, rulesetKeyDestruct, strcasecmp));
	conf->rulesets.pDflt = NULL;

finalize_it:
	RETiRet;
}

/* this is a special destructor for the linkedList class. LinkedList does NOT
 * provide a pointer to the pointer, but rather the raw pointer itself. So we 
 * must map this, otherwise the destructor will abort.
 */
rsRetVal
rulesetDestructForLinkedList(void *pData)
{
	ruleset_t *pThis = (ruleset_t*) pData;
	return rulesetDestruct(&pThis);
}

/* helper for debugPrint(), initiates rule printing */
DEFFUNC_llExecFunc(doDebugPrintRule)
{
	return rule.DebugPrint((rule_t*) pData);
}
/* debugprint for the ruleset object */
BEGINobjDebugPrint(ruleset) /* be sure to specify the object type also in END and CODESTART macros! */
CODESTARTobjDebugPrint(ruleset)
	dbgoprint((obj_t*) pThis, "rsyslog ruleset %s:\n", pThis->pszName);
	llExecFunc(&pThis->llRules, doDebugPrintRule, NULL);
ENDobjDebugPrint(ruleset)


/* helper for debugPrintAll(), prints a single ruleset */
DEFFUNC_llExecFunc(doDebugPrintAll)
{
	return rulesetDebugPrint((ruleset_t*) pData);
}
/* debug print all rulesets
 */
static rsRetVal
debugPrintAll(rsconf_t *conf)
{
	DEFiRet;
	dbgprintf("All Rulesets:\n");
	llExecFunc(&(conf->rulesets.llRulesets), doDebugPrintAll, NULL);
	dbgprintf("End of Rulesets.\n");
	RETiRet;
}


/* Create a ruleset-specific "main" queue for this ruleset. If one is already
 * defined, an error message is emitted but nothing else is done.
 * Note: we use the main message queue parameters for queue creation and access
 * syslogd.c directly to obtain these. This is far from being perfect, but
 * considered acceptable for the time being.
 * rgerhards, 2009-10-27
 */
static inline rsRetVal
doRulesetCreateQueue(rsconf_t *conf, int *pNewVal)
{
	uchar *rsname;
	DEFiRet;

	if(conf->rulesets.pCurr == NULL) {
		errmsg.LogError(0, RS_RET_NO_CURR_RULESET, "error: currently no specific ruleset specified, thus a "
				"queue can not be added to it");
		ABORT_FINALIZE(RS_RET_NO_CURR_RULESET);
	}

	if(conf->rulesets.pCurr->pQueue != NULL) {
		errmsg.LogError(0, RS_RET_RULES_QUEUE_EXISTS, "error: ruleset already has a main queue, can not "
				"add another one");
		ABORT_FINALIZE(RS_RET_RULES_QUEUE_EXISTS);
	}

	if(pNewVal == 0)
		FINALIZE; /* if it is turned off, we do not need to change anything ;) */

	rsname = (conf->rulesets.pCurr->pszName == NULL) ? (uchar*) "[ruleset]" : conf->rulesets.pCurr->pszName;
	DBGPRINTF("adding a ruleset-specific \"main\" queue for ruleset '%s'\n", rsname);
	CHKiRet(createMainQueue(&conf->rulesets.pCurr->pQueue, rsname));

finalize_it:
	RETiRet;
}

static rsRetVal
rulesetCreateQueue(void __attribute__((unused)) *pVal, int *pNewVal)
{
	return doRulesetCreateQueue(ourConf, pNewVal);
}

/* Add a ruleset specific parser to the ruleset. Note that adding the first
 * parser automatically disables the default parsers. If they are needed as well,
 * the must be added via explicit config directives.
 * Note: this is the only spot in the code that requires the parser object. In order
 * to solve some class init bootstrap sequence problems, we get the object handle here
 * instead of during module initialization. Note that objUse() is capable of being 
 * called multiple times.
 * rgerhards, 2009-11-04
 */
static rsRetVal
doRulesetAddParser(rsconf_t *conf, uchar *pName)
{
	parser_t *pParser;
	DEFiRet;

	assert(conf->rulesets.pCurr != NULL); 

	CHKiRet(objUse(parser, CORE_COMPONENT));
	iRet = parser.FindParser(&pParser, pName);
	if(iRet == RS_RET_PARSER_NOT_FOUND) {
		errmsg.LogError(0, RS_RET_PARSER_NOT_FOUND, "error: parser '%s' unknown at this time "
			  	"(maybe defined too late in rsyslog.conf?)", pName);
		ABORT_FINALIZE(RS_RET_NO_CURR_RULESET);
	} else if(iRet != RS_RET_OK) {
		errmsg.LogError(0, iRet, "error trying to find parser '%s'\n", pName);
		FINALIZE;
	}

	CHKiRet(parser.AddParserToList(&conf->rulesets.pCurr->pParserLst, pParser));

	DBGPRINTF("added parser '%s' to ruleset '%s'\n", pName, conf->rulesets.pCurr->pszName);

finalize_it:
	d_free(pName); /* no longer needed */

	RETiRet;
}

static rsRetVal
rulesetAddParser(void __attribute__((unused)) *pVal, uchar *pName)
{
	return doRulesetAddParser(ourConf, pName);
}


/* queryInterface function
 * rgerhards, 2008-02-21
 */
BEGINobjQueryInterface(ruleset)
CODESTARTobjQueryInterface(ruleset)
	if(pIf->ifVersion != rulesetCURR_IF_VERSION) { /* check for current version, increment on each change */
		ABORT_FINALIZE(RS_RET_INTERFACE_NOT_SUPPORTED);
	}

	/* ok, we have the right interface, so let's fill it
	 * Please note that we may also do some backwards-compatibility
	 * work here (if we can support an older interface version - that,
	 * of course, also affects the "if" above).
	 */
	pIf->Construct = rulesetConstruct;
	pIf->ConstructFinalize = rulesetConstructFinalize;
	pIf->Destruct = rulesetDestruct;
	pIf->DebugPrint = rulesetDebugPrint;

	pIf->IterateAllActions = iterateAllActions;
	pIf->DestructAllActions = destructAllActions;
	pIf->AddRule = addRule;
	pIf->AddScript = addScript;
	pIf->ProcessBatch = processBatch;
	pIf->SetName = setName;
	pIf->DebugPrintAll = debugPrintAll;
	pIf->GetCurrent = GetCurrent;
	pIf->GetRuleset = rulesetGetRuleset;
	pIf->SetDefaultRuleset = SetDefaultRuleset;
	pIf->SetCurrRuleset = SetCurrRuleset;
	pIf->GetRulesetQueue = GetRulesetQueue;
	pIf->GetParserList = GetParserList;
finalize_it:
ENDobjQueryInterface(ruleset)


/* Exit the ruleset class.
 * rgerhards, 2009-04-06
 */
BEGINObjClassExit(ruleset, OBJ_IS_CORE_MODULE) /* class, version */
	objRelease(errmsg, CORE_COMPONENT);
	objRelease(rule, CORE_COMPONENT);
	objRelease(parser, CORE_COMPONENT);
ENDObjClassExit(ruleset)


/* Initialize the ruleset class. Must be called as the very first method
 * before anything else is called inside this class.
 * rgerhards, 2008-02-19
 */
BEGINObjClassInit(ruleset, 1, OBJ_IS_CORE_MODULE) /* class, version */
	/* request objects we use */
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(rule, CORE_COMPONENT));

	/* set our own handlers */
	OBJSetMethodHandler(objMethod_DEBUGPRINT, rulesetDebugPrint);
	OBJSetMethodHandler(objMethod_CONSTRUCTION_FINALIZER, rulesetConstructFinalize);

	/* config file handlers */
	CHKiRet(regCfSysLineHdlr((uchar *)"rulesetparser", 0, eCmdHdlrGetWord, rulesetAddParser, NULL, NULL));
	CHKiRet(regCfSysLineHdlr((uchar *)"rulesetcreatemainqueue", 0, eCmdHdlrBinary, rulesetCreateQueue, NULL, NULL));
ENDObjClassInit(ruleset)

/* vi:set ai:
 */
