/*
 * RtmpPlayer2.cpp
 *
 *  Created on: 2016Äê11ÔÂ29ÈÕ
 *      Author: xzl
 */

#include "RtmpPlayer.h"
#include "Rtsp/Rtsp.h"
#include "Rtmp/utils.h"
#include "Util/util.h"
#include "Util/onceToken.h"
#include "Thread/ThreadPool.h"

using namespace ZL::Util;

namespace ZL {
namespace Rtmp {

unordered_map<string, RtmpPlayer::rtmpCMDHandle> RtmpPlayer::g_mapCmd;
RtmpPlayer::RtmpPlayer() {
	static onceToken token([]() {
		g_mapCmd.emplace("_error",&RtmpPlayer::onCmd_result);
		g_mapCmd.emplace("_result",&RtmpPlayer::onCmd_result);
		g_mapCmd.emplace("onStatus",&RtmpPlayer::onCmd_onStatus);
		g_mapCmd.emplace("onMetaData",&RtmpPlayer::onCmd_onMetaData);
		}, []() {});

}

RtmpPlayer::~RtmpPlayer() {
	teardown();
	DebugL << endl;
}
void RtmpPlayer::teardown() {
	if (alive()) {
		m_strApp.clear();
		m_strStream.clear();
		m_strTcUrl.clear();
		m_mapOnResultCB.clear();
        {
            lock_guard<recursive_mutex> lck(m_mtxDeque);
            m_dqOnStatusCB.clear();
        }
		m_pBeatTimer.reset();
		m_pPlayTimer.reset();
		m_pMediaTimer.reset();
        m_fSeekTo = 0;
        CLEAR_ARR(m_adFistStamp);
        CLEAR_ARR(m_adNowStamp);
        clear();
        shutdown();
	}
}

void RtmpPlayer::play(const char* strUrl, const char * , const char *, eRtpType)  {
	teardown();
	string strHost = FindField(strUrl, "://", "/");
	m_strApp = 	FindField(strUrl, (strHost + "/").data(), "/");
    m_strStream = FindField(strUrl, (strHost + "/" + m_strApp + "/").data(), NULL);
    m_strTcUrl = string("rtmp://") + strHost + "/" + m_strApp;

    if (!m_strApp.size() || !m_strStream.size()) {
        _onPlayResult(SockException(Err_other,"rtmp url非法"));
        return;
    }
	DebugL << strHost << " " << m_strApp << " " << m_strStream;

	auto iPort = atoi(FindField(strHost.c_str(), ":", NULL).c_str());
	if (iPort <= 0) {
        //rtmp 默认端口1935
		iPort = 1935;
	} else {
        //服务器域名
		strHost = FindField(strHost.c_str(), NULL, ":");
	}
	startConnect(strHost, iPort);
}
void RtmpPlayer::onErr(const SockException &ex){
	_onShutdown(ex);
}
void RtmpPlayer::onConnect(const SockException &err){
	if(err.getErrCode()!=Err_success) {
		_onPlayResult(err);
		return;
	}

	weak_ptr<RtmpPlayer> weakSelf= dynamic_pointer_cast<RtmpPlayer>(shared_from_this());
	m_pPlayTimer.reset( new Timer(10, [weakSelf]() {
		auto strongSelf=weakSelf.lock();
		if(!strongSelf) {
			return false;
		}
		strongSelf->_onPlayResult(SockException(Err_timeout,"play rtmp timeout"));
		strongSelf->teardown();
		return false;
	}));
	startClientSession([weakSelf](){
        auto strongSelf=weakSelf.lock();
		if(!strongSelf) {
            return;
        }
		strongSelf->send_connect();
	});
}
void RtmpPlayer::onRecv(const Socket::Buffer::Ptr &pBuf){
	try {
		onParseRtmp(pBuf->data(), pBuf->size());
	} catch (exception &e) {
		SockException ex(Err_other, e.what());
		_onPlayResult(ex);
		_onShutdown(ex);
		teardown();
	}
}

void RtmpPlayer::pause(bool bPause) {
	send_pause(bPause);
}

inline void RtmpPlayer::send_connect() {
	AMFValue obj(AMF_OBJECT);
	obj.set("app", m_strApp);
	obj.set("tcUrl", m_strTcUrl);
	obj.set("fpad", false);
	obj.set("capabilities", 15);
	obj.set("videoFunction", 1);
    //只支持aac
    obj.set("audioCodecs", 3191);
    //只支持H264
    obj.set("videoCodecs", 252);
	sendInvoke("connect", obj);
	addOnResultCB([this](AMFDecoder &dec){
		//TraceL << "connect result";
		dec.load<AMFValue>();
		auto val = dec.load<AMFValue>();
		auto level = val["level"].as_string();
		auto code = val["code"].as_string();
		if(level != "status"){
			throw std::runtime_error(StrPrinter <<"connect 失败：" << level << " " << code << endl);
		}
		send_createStream();
	});
}

inline void RtmpPlayer::send_createStream() {
	AMFValue obj(AMF_NULL);
	sendInvoke("createStream", obj);
	addOnResultCB([this](AMFDecoder &dec){
		//TraceL << "createStream result";
		dec.load<AMFValue>();
		m_ui32StreamId = dec.load<int>();
		send_play();
	});
}

inline void RtmpPlayer::send_play() {
	AMFEncoder enc;
	enc << "play" << ++m_iReqID  << nullptr << m_strStream << (double)m_ui32StreamId;
	sendRequest(MSG_CMD, enc.data());
	auto fun = [this](AMFValue &val){
		//TraceL << "play onStatus";
		auto level = val["level"].as_string();
		auto code = val["code"].as_string();
		if(level != "status"){
			throw std::runtime_error(StrPrinter <<"play 失败：" << level << " " << code << endl);
		}
	};
	addOnStatusCB(fun);
	addOnStatusCB(fun);
}

inline void RtmpPlayer::send_pause(bool bPause) {
	AMFEncoder enc;
	enc << "pause" << ++m_iReqID  << nullptr << bPause;
	sendRequest(MSG_CMD, enc.data());
	auto fun = [this,bPause](AMFValue &val){
        //TraceL << "pause onStatus";
        auto level = val["level"].as_string();
        auto code = val["code"].as_string();
        if(level != "status") {
            if(!bPause){
                throw std::runtime_error(StrPrinter <<"pause 恢复播放失败：" << level << " " << code << endl);
            }
        }else{
            m_bPaused = bPause;
            if(!bPause){
                _onPlayResult(SockException(Err_success, "rtmp resum success"));
            }else{
                //暂停播放
                m_pMediaTimer.reset();
            }
        }
	};
	addOnStatusCB(fun);

	m_pBeatTimer.reset();
	if(bPause){
		weak_ptr<RtmpPlayer> weakSelf = dynamic_pointer_cast<RtmpPlayer>(shared_from_this());
		m_pBeatTimer.reset(new Timer(3,[weakSelf](){
			auto strongSelf = weakSelf.lock();
			if (!strongSelf){
				return false;
			}
			uint32_t timeStamp = ::time(NULL);
			strongSelf->sendUserControl(CONTROL_PING_REQUEST, timeStamp);
			return true;
		}));
	}
}

void RtmpPlayer::onCmd_result(AMFDecoder &dec){
	auto iReqId = dec.load<int>();
	auto it = m_mapOnResultCB.find(iReqId);
	if(it != m_mapOnResultCB.end()){
		it->second(dec);
		m_mapOnResultCB.erase(it);
	}else{
		WarnL << "unhandled _result";
	}
}
void RtmpPlayer::onCmd_onStatus(AMFDecoder &dec) {
	AMFValue val;
	while(true){
		val = dec.load<AMFValue>();
		if(val.type() == AMF_OBJECT){
			break;
		}
	}
	if(val.type() != AMF_OBJECT){
		throw std::runtime_error("onStatus: 未找到结果对象");
	}
    
    lock_guard<recursive_mutex> lck(m_mtxDeque);
	if(m_dqOnStatusCB.size()){
		m_dqOnStatusCB.front()(val);
		m_dqOnStatusCB.pop_front();
	}else{
		auto level = val["level"];
		auto code = val["code"].as_string();
		if(level.type() == AMF_STRING){
			if(level.as_string() != "status"){
				throw std::runtime_error(StrPrinter <<"onStatus 失败:" << level.as_string() << " " << code << endl);
			}
		}
		//WarnL << "unhandled onStatus:" << code;
    }
}

void RtmpPlayer::onCmd_onMetaData(AMFDecoder &dec) {
	//TraceL;
	auto val = dec.load<AMFValue>();
	if(!onCheckMeta(val)){
		throw std::runtime_error("onCheckMeta faied");
	}
	_onPlayResult(SockException(Err_success,"play rtmp success"));
}

void RtmpPlayer::onStreamDry(uint32_t ui32StreamId) {
	//TraceL << ui32StreamId;
	_onShutdown(SockException(Err_other,"rtmp stream dry"));
}


void RtmpPlayer::onRtmpChunk(RtmpPacket &chunkData) {
	switch (chunkData.typeId) {
		case MSG_CMD:
		case MSG_CMD3:
		case MSG_DATA:
		case MSG_DATA3: {
			AMFDecoder dec(chunkData.strBuf, 0);
			std::string type = dec.load<std::string>();
			auto it = g_mapCmd.find(type);
			if(it != g_mapCmd.end()){
				auto fun = it->second;
				(this->*fun)(dec);
			}else{
				WarnL << "can not support cmd:" << type;
			}
		}
			break;
		case MSG_AUDIO:
		case MSG_VIDEO: {
            auto idx = chunkData.typeId%2;
            if (m_aNowStampTicker[idx].elapsedTime() > 500) {
                m_adNowStamp[idx] = chunkData.timeStamp;
            }
			_onMediaData(chunkData);
		}
			break;
		default:
			//WarnL << "unhandled message:" << (int) chunkData.typeId << hexdump(chunkData.strBuf.data(), chunkData.strBuf.size());
			break;
		}
}

float RtmpPlayer::getProgressTime() const{
    double iTime[2] = {0,0};
    for(auto i = 0 ;i < 2 ;i++){
        iTime[i] = (m_adNowStamp[i] - m_adFistStamp[i]) / 1000.0;
    }
    return m_fSeekTo + MAX(iTime[0],iTime[1]);
}
void RtmpPlayer::seekToTime(float fTime){
    if (m_bPaused) {
        pause(false);
    }
    AMFEncoder enc;
    enc << "seek" << ++m_iReqID << nullptr << fTime * 1000.0;
    sendRequest(MSG_CMD, enc.data());
    addOnStatusCB([this,fTime](AMFValue &val) {
        //TraceL << "seek result";
        m_aNowStampTicker[0].resetTime();
        m_aNowStampTicker[1].resetTime();
        float iTimeInc = fTime - getProgressTime();
        for(auto i = 0 ;i < 2 ;i++){
            m_adFistStamp[i] = m_adNowStamp[i] + iTimeInc * 1000.0;
            m_adNowStamp[i] = m_adFistStamp[i];
        }
        m_fSeekTo = fTime;
    });

}

} /* namespace Rtmp */
} /* namespace ZL */

