

#include <hipcdata.h>
#include "serverServer.h"
#include "../common/middleConfig.h"
#include <hlog.h>

static constexpr HUINT SC_MSG_BEGINID = 88;
static constexpr HUINT SC_MSG_BACK = 288;

CServerServer::CServerServer(HCSTRR strSid, HCSTRR strCid) 
	: m_strSid(strSid), m_strCid(strCid){

	m_sid = HCStr::stoi(strSid);
	assert(m_sid < CShmInfo::HSI_MAX_COUNT);

	m_cid = HCStr::stoi(m_strCid);
	assert(m_cid < CShmInfo::HSI_MAX_COUNT);

}


CServerServer::~CServerServer() {

}


HRET CServerServer::Init () throw (HCException) {
	/**
	   > init log
	   > load xml config.
	   > init shminfo, mq;
	   > Write process information;
	   > Load so_conf, so;
	   > compare so funs with global funs;
	   > Run
	       a. heartBeat;
	       b. dealCmd;
	**/	

	// get the process id
	HNOTOK_RETURN(setupParam());

	HNOTOK_RETURN(initShmInfo ());

	HNOTOK_RETURN(checkConf ());	

	HNOTOK_RETURN(initMq ());

	HNOTOK_RETURN(readConf ());	

	HNOTOK_RETURN(initSo() );

	// compare funs;

	HRETURN_OK;

}


HRET CServerServer::RunServer () throw (HCException) {

	HFUN_BEGIN;

	HRET ret = HERR_NO(OK);

	while (1) {

		try {			
		
			ret = runMsg();
			HIF_NOTOK(ret) {

				if (ret != HERR_NO(TIME_OUT)) {

					LOG_ES("Recv message queue failed");					
					break;
				
				} else {

					LOG_WS("Recv message queue timeout");

				}
			
			}

			ret = runHeartBeat();
			HIF_OK(ret) {
				break;
			}
		} catch (HCException& ex) {

			LOG_ERROR("CServerServer::RunServer Get a error: [%s]", ex.what());
			ret = hlast_err();
			break;
			
		}
		
	}

	HFUN_END;

	return ret;
}


HRET CServerServer::setupParam () {

	IF_FALSE(middle_config->HasSo(m_cid, m_strSoName)) {

		HRETURN(INVL_PARA);

	}

	m_conf_key = middle_config->GetInt("conf_key", 2688);
	m_app_count = middle_config->GetInt("app_count", 32);
	
	m_mq_key = middle_config->GetInt("mq_key", 1688);
	m_mq_bk_count = middle_config->GetInt("mq_bk_count", 8);
	
	m_mq_valid_time = middle_config->GetInt("mq_validTime", 600);
	m_mq_try_time = middle_config->GetInt("mq_tryTime", 5);
	m_sleep_time = middle_config->GetInt("mq_sleepTime", 0);
	m_sleep_timeu = middle_config->GetInt("mq_SleepTimeu", 5);

	LOG_NORMAL("conf_key:[%d], app_count:[%d], mq_key:[%d], mq_bk_count:[%d]",
		m_conf_key, m_app_count, m_mq_key, m_mq_bk_count);

	LOG_NORMAL("mq_valid_time:[%d], mq_try_time:[%d], m_sleep_time:[%d], sleep_timeu:[%d]",
		m_mq_valid_time, m_mq_try_time, m_sleep_time, m_sleep_timeu);

	HRETURN_OK;

}


HRET CServerServer::checkConf () {

	m_pid = getpid();

	m_shmIndex = m_pShmInfo->NewPidState(m_sid, m_cid, m_pid);
	LOG_NORMAL("newpidstate, sid:[%d], cid:[%d], pid:[%d], return_index:[%d]",
		m_sid, m_cid, m_pid, m_shmIndex);

	const CINFO* pInfo = m_pShmInfo->GetInfo(m_shmIndex);
	HASSERT_THROW(pInfo != nullptr, INVL_PARA);

	LOG_NORMAL("return info: index[%d], sid[%d], cid[%d], ctime[%d], access[%d], pid[%d]", 
		pInfo->GetIndex(), pInfo->GetSid(), pInfo->GetCid(), 
		pInfo->GetCTime(), pInfo->GetAccess(), pInfo->GetPid());

	HASSERT_THROW(pInfo->GetSid() == m_sid, INVL_PARA);
	
	HASSERT_THROW(pInfo->GetCid() == m_cid, INVL_PARA);

	m_pShmInfo->UpdateContinue(m_shmIndex);
	
	
	HRETURN_OK;
}


HRET CServerServer::runMsg () throw (HCException) {

	// recv request;
	HNOTOK_RETURN(recvRequest ());

	LOG_NS("success get a request");
	
	// parse command on request;
	std::vector<HCIpcData> req_datas;
 	HNOTOK_RETURN(HCIpcData::ParseDatas(m_buf, m_ctl.GetLen(), req_datas));
	if (req_datas.size() != SERVER_REQ_PS_COUNT) {
		HRETURN(INVL_PARA);
	}

	LOG_NORMAL("get a request with %d command", req_datas.size());

	// command, flags<need_return,...>, param, 
	const HCIpcData& _cmd_data = req_datas[0];
	LOG_NORMAL("command name: [%s]", _cmd_data.GetString().c_str());
	declare_fun_t call_fun = m_so.GetCallFun(_cmd_data.GetString());

	if (call_fun == nullptr) {
	    LOG_ERROR("not this command [%s]", _cmd_data.GetString().c_str());

	    
	    HRETURN(INVL_PARA);
	}

	const HCIpcData& _ps_data = req_datas[1];

	// exec command;	
	m_data.idata = _ps_data.CBegin();
	m_data.ilen = _ps_data.GetDataLen();

	LOG_NORMAL("call function [%p] with [%d] byts", (void*)call_fun, m_data.ilen);
	
	if (call_fun != nullptr) {
	    auto cb = call_fun(&m_data);
	    LOG_NORMAL("%s return: [%d]", _cmd_data.GetString().c_str(), cb);
	}

	HNOTOK_RETURN(sendResponse());

	// return
	HRETURN_OK;
}

HRET CServerServer::runHeartBeat () throw (HCException) {

	return  m_pShmInfo->UpdateContinue(m_shm_index);


}

HRET CServerServer::recvRequest () throw (HCException) {

    // set type 0 to recv all type message.
    //m_ctl.SetType(SC_MSG_BEGINID + m_cid);
    m_ctl.SetType(1);
    m_ctl.SetNid(m_cid);
    m_ctl.SetLen (BUF_LEN);
    m_ctl.SetCTime(time(nullptr));
	
    memset(m_buf, 0, BUF_LEN);
    	
    HRET ret = m_pMq->Recv(m_ctl, m_buf, TIMEOUT, 0);

    LOG_NORMAL("Read MQ for Control{type:[%d], nid:[%d], bid:[%d], len:[%d], rtype: [%d]}",
	       m_ctl.GetType(), m_ctl.GetNid(), m_ctl.GetBid(), m_ctl.GetLen(), m_ctl.GetRType());

    return ret;

}


HRET CServerServer::sendResponse () throw (HCException) {

    LOG_NORMAL("sendResponse: [%s]", m_data.odata);

    // note: return back message for type.
    //m_ctl.SetType(SC_MSG_BACK + m_cid);
    m_ctl.SetReturn();

    /*****************************/
    // TODO: change back cid
    /*****************************/
    m_ctl.SetNid(m_cid);
    m_ctl.SetCTime(time(nullptr));

    std::vector<HSTR> strs;
    strs.push_back(m_data.odata);
    HUINT len = 0;
    HPSZ sbuf = HCIpcData::MakeBuf(strs, len);

    LOG_NORMAL("return [%d] bytes to client", len);

    m_ctl.SetLen (len);
    HRET ret = m_pMq->Send(m_ctl, sbuf, 0);

    LOG_NORMAL("message queue send return: [%d]", ret);
    LOG_NORMAL("return send control: {type: [%d], nid: [%d], bid: [%d], len: [%d], rtype: [%d]}", m_ctl.GetType(), m_ctl.GetNid(), m_ctl.GetBid(), m_ctl.GetLen(), m_ctl.GetRType());
       

    free(sbuf);

    return ret;

}


HRET CServerServer::readConf () throw (HCException) {

	IF_FALSE(middle_config->HasSo(m_strSoName)) {

		HRETURN(INVL_PARA);

	}

	m_soEntry = middle_config->GetEntry(m_strSoName);
	
	LOG_NORMAL("Find so entry for [%s]. index:[%d], name:[%s], pcount:[%d], tcount:[%d], path:[%s]",
		m_strSoName.c_str(), m_soEntry.m_index, m_soEntry.m_name.c_str(),
		m_soEntry.m_pcount, m_soEntry.m_tcount, m_soEntry.m_path.c_str());
	
	HRETURN_OK;
}


HRET CServerServer::initShmInfo () throw (HCException) {

	HFUN_BEGIN;

	m_pShmInfo = new CShmInfo(m_conf_key, m_app_count);
	assert(m_pShmInfo);

	HNOTOK_RETURN(m_pShmInfo->Open());

	HFUN_END;

	HRETURN_OK;
}


HRET CServerServer::initMq () throw (HCException) {
	HFUN_BEGIN;

	m_pMq = new HCMq(m_mq_key, m_app_count, m_mq_bk_count,
		HCMq::SShmControl(m_mq_valid_time, m_mq_try_time, m_sleep_time,
		m_sleep_timeu));
	assert(m_pMq);

	HNOTOK_RETURN(m_pMq->Open());

	HFUN_END;
	HRETURN_OK;

}

HRET CServerServer::initSo () throw (HCException) {
	HFUN_BEGIN;

	m_so.SetSoFile(HCFileName(m_soEntry.m_path));

	HNOTOK_RETURN(m_so.Open());

	HFUN_END;
	HRETURN_OK;
}




