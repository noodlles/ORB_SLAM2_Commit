/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/


#include "Tracking.h"

#include<opencv2/core/core.hpp>
#include<opencv2/features2d/features2d.hpp>

#include"ORBmatcher.h"
#include"FrameDrawer.h"
#include"Converter.h"
#include"Map.h"
#include"Initializer.h"

#include"Optimizer.h"
#include"PnPsolver.h"

#include<iostream>

#include<mutex>


using namespace std;

namespace ORB_SLAM2
{

Tracking::Tracking(System *pSys, ORBVocabulary* pVoc, FrameDrawer *pFrameDrawer, MapDrawer *pMapDrawer, Map *pMap, KeyFrameDatabase* pKFDB, const string &strSettingPath, const int sensor):
    mState(NO_IMAGES_YET), mSensor(sensor), mbOnlyTracking(false), mbVO(false), mpORBVocabulary(pVoc),
    mpKeyFrameDB(pKFDB), mpInitializer(static_cast<Initializer*>(NULL)), mpSystem(pSys), mpViewer(NULL),
    mpFrameDrawer(pFrameDrawer), mpMapDrawer(pMapDrawer), mpMap(pMap), mnLastRelocFrameId(0)
{
    // Load camera parameters from settings file

    cv::FileStorage fSettings(strSettingPath, cv::FileStorage::READ);
    float fx = fSettings["Camera.fx"];
    float fy = fSettings["Camera.fy"];
    float cx = fSettings["Camera.cx"];
    float cy = fSettings["Camera.cy"];

    cv::Mat K = cv::Mat::eye(3,3,CV_32F);
    K.at<float>(0,0) = fx;
    K.at<float>(1,1) = fy;
    K.at<float>(0,2) = cx;
    K.at<float>(1,2) = cy;
    K.copyTo(mK);

    cv::Mat DistCoef(4,1,CV_32F);
    DistCoef.at<float>(0) = fSettings["Camera.k1"];
    DistCoef.at<float>(1) = fSettings["Camera.k2"];
    DistCoef.at<float>(2) = fSettings["Camera.p1"];
    DistCoef.at<float>(3) = fSettings["Camera.p2"];
    const float k3 = fSettings["Camera.k3"];
    if(k3!=0)
    {
        DistCoef.resize(5);
        DistCoef.at<float>(4) = k3;
    }
    DistCoef.copyTo(mDistCoef);

    mbf = fSettings["Camera.bf"];

    float fps = fSettings["Camera.fps"];
    if(fps==0)
        fps=30;

    // Max/Min Frames to insert keyframes and to check relocalisation
    mMinFrames = 0;
    mMaxFrames = fps;

    cout << endl << "Camera Parameters: " << endl;
    cout << "- fx: " << fx << endl;
    cout << "- fy: " << fy << endl;
    cout << "- cx: " << cx << endl;
    cout << "- cy: " << cy << endl;
    cout << "- k1: " << DistCoef.at<float>(0) << endl;
    cout << "- k2: " << DistCoef.at<float>(1) << endl;
    if(DistCoef.rows==5)
        cout << "- k3: " << DistCoef.at<float>(4) << endl;
    cout << "- p1: " << DistCoef.at<float>(2) << endl;
    cout << "- p2: " << DistCoef.at<float>(3) << endl;
    cout << "- fps: " << fps << endl;


    int nRGB = fSettings["Camera.RGB"];
    mbRGB = nRGB;

    if(mbRGB)
        cout << "- color order: RGB (ignored if grayscale)" << endl;
    else
        cout << "- color order: BGR (ignored if grayscale)" << endl;

    // Load ORB parameters

    int nFeatures = fSettings["ORBextractor.nFeatures"];
    float fScaleFactor = fSettings["ORBextractor.scaleFactor"];
    int nLevels = fSettings["ORBextractor.nLevels"];
    int fIniThFAST = fSettings["ORBextractor.iniThFAST"];
    int fMinThFAST = fSettings["ORBextractor.minThFAST"];

    //新建ORBextractor对象，执行其构造函数
    mpORBextractorLeft = new ORBextractor(nFeatures,fScaleFactor,nLevels,fIniThFAST,fMinThFAST);

    if(sensor==System::STEREO)
        mpORBextractorRight = new ORBextractor(nFeatures,fScaleFactor,nLevels,fIniThFAST,fMinThFAST);

    if(sensor==System::MONOCULAR)
        mpIniORBextractor = new ORBextractor(2*nFeatures,fScaleFactor,nLevels,fIniThFAST,fMinThFAST);

    cout << endl  << "ORB Extractor Parameters: " << endl;
    cout << "- Number of Features: " << nFeatures << endl;
    cout << "- Scale Levels: " << nLevels << endl;
    cout << "- Scale Factor: " << fScaleFactor << endl;
    cout << "- Initial Fast Threshold: " << fIniThFAST << endl;
    cout << "- Minimum Fast Threshold: " << fMinThFAST << endl;

    //如果是双目或者RGBD，需要计算mThDepth
    if(sensor==System::STEREO || sensor==System::RGBD)
    {
        mThDepth = mbf*(float)fSettings["ThDepth"]/fx;
        cout << endl << "Depth Threshold (Close/Far Points): " << mThDepth << endl;
    }

    if(sensor==System::RGBD)
    {
        mDepthMapFactor = fSettings["DepthMapFactor"];
        if(fabs(mDepthMapFactor)<1e-5)
            mDepthMapFactor=1;
        else
            mDepthMapFactor = 1.0f/mDepthMapFactor;
    }

}

void Tracking::SetLocalMapper(LocalMapping *pLocalMapper)
{
    mpLocalMapper=pLocalMapper;
}

void Tracking::SetLoopClosing(LoopClosing *pLoopClosing)
{
    mpLoopClosing=pLoopClosing;
}

void Tracking::SetViewer(Viewer *pViewer)
{
    mpViewer=pViewer;
}


cv::Mat Tracking::GrabImageStereo(const cv::Mat &imRectLeft, const cv::Mat &imRectRight, const double &timestamp)
{
    mImGray = imRectLeft;
    cv::Mat imGrayRight = imRectRight;
    //将图片转化为灰度图
    if(mImGray.channels()==3)
    {
        if(mbRGB)
        {
            cvtColor(mImGray,mImGray,CV_RGB2GRAY);
            cvtColor(imGrayRight,imGrayRight,CV_RGB2GRAY);
        }
        else
        {
            cvtColor(mImGray,mImGray,CV_BGR2GRAY);
            cvtColor(imGrayRight,imGrayRight,CV_BGR2GRAY);
        }
    }
    else if(mImGray.channels()==4)
    {
        if(mbRGB)
        {
            cvtColor(mImGray,mImGray,CV_RGBA2GRAY);
            cvtColor(imGrayRight,imGrayRight,CV_RGBA2GRAY);
        }
        else
        {
            cvtColor(mImGray,mImGray,CV_BGRA2GRAY);
            cvtColor(imGrayRight,imGrayRight,CV_BGRA2GRAY);
        }
    }

    //构造函数是stereo版本的
    mCurrentFrame = Frame(mImGray,imGrayRight,timestamp,mpORBextractorLeft,mpORBextractorRight,mpORBVocabulary,mK,mDistCoef,mbf,mThDepth);

    Track();

    return mCurrentFrame.mTcw.clone();
}


cv::Mat Tracking::GrabImageRGBD(const cv::Mat &imRGB,const cv::Mat &imD, const double &timestamp)
{
    mImGray = imRGB;
    cv::Mat imDepth = imD;

    if(mImGray.channels()==3)
    {
        if(mbRGB)
            cvtColor(mImGray,mImGray,CV_RGB2GRAY);
        else
            cvtColor(mImGray,mImGray,CV_BGR2GRAY);
    }
    else if(mImGray.channels()==4)
    {
        if(mbRGB)
            cvtColor(mImGray,mImGray,CV_RGBA2GRAY);
        else
            cvtColor(mImGray,mImGray,CV_BGRA2GRAY);
    }

    if((fabs(mDepthMapFactor-1.0f)>1e-5) || imDepth.type()!=CV_32F)
        imDepth.convertTo(imDepth,CV_32F,mDepthMapFactor);

    mCurrentFrame = Frame(mImGray,imDepth,timestamp,mpORBextractorLeft,mpORBVocabulary,mK,mDistCoef,mbf,mThDepth);

    Track();

    return mCurrentFrame.mTcw.clone();
}


cv::Mat Tracking::GrabImageMonocular(const cv::Mat &im, const double &timestamp)
{
    mImGray = im;

	  //将图片转化为灰度图
    if(mImGray.channels()==3)
    {
        if(mbRGB)
            cvtColor(mImGray,mImGray,CV_RGB2GRAY);
        else
            cvtColor(mImGray,mImGray,CV_BGR2GRAY);
    }
    else if(mImGray.channels()==4)
    {
        if(mbRGB)
            cvtColor(mImGray,mImGray,CV_RGBA2GRAY);
        else
            cvtColor(mImGray,mImGray,CV_BGRA2GRAY);
    }
    
    //如果tracking没有初始化，或者没有图片（也是没有初始化），则新建一个Frame对象
    //使用了不同的```ORBextractor```来构建```Frame```，是应为在初始化阶段的帧需要跟多的特征点
    if(mState==NOT_INITIALIZED || mState==NO_IMAGES_YET)
        mCurrentFrame = Frame(mImGray,timestamp,mpIniORBextractor,mpORBVocabulary,mK,mDistCoef,mbf,mThDepth);
    else
        mCurrentFrame = Frame(mImGray,timestamp,mpORBextractorLeft,mpORBVocabulary,mK,mDistCoef,mbf,mThDepth);

    //跟踪
    Track();

    //返回跟踪结果
    return mCurrentFrame.mTcw.clone();
}

void Tracking:: Track()
{
    // 如果图像复位过、或者第一次运行，则为NO_IMAGE_YET状态
    if(mState==NO_IMAGES_YET)
    {
        mState = NOT_INITIALIZED;
    }

    mLastProcessedState=mState;

    // Get Map Mutex -> Map cannot be changed
    // 线程锁, 锁定地图,此时不允许地图更新
    unique_lock<mutex> lock(mpMap->mMutexMapUpdate);

    //如果tracking没有初始化，则初始化
    if(mState==NOT_INITIALIZED)
    {
        if(mSensor==System::STEREO || mSensor==System::RGBD)
            StereoInitialization();
        else
            MonocularInitialization();

        mpFrameDrawer->Update(this);

        //tracking初始化完成之后, mState=OK
        if(mState!=OK)
            return;
    }
    else
    //tracking初始化成功后
    {
        // System is initialized. Track Frame.
        // bOK为临时变量，bOK==true现在tracking状态正常,能够及时的反应现在tracking的状态
        bool bOK;

        // Initial camera pose estimation using motion model or relocalization (if tracking is lost)
        // 初始化相机位姿,使用运动模型或者重定位
        // 用户可以通过在viewer中的开关menuLocalizationMode，控制是否ActivateLocalizationMode，并最终管控mbOnlyTracking是否为true
        // mbOnlyTracking等于false表示正常VO模式（有地图更新），mbOnlyTracking等于true表示用户手动选择定位模式
        if(!mbOnlyTracking)
        {
            // Local Mapping is activated. This is the normal behaviour, unless
            // you explicitly activate the "only tracking" mode.
            // 进入正常SLAM,不是定位模式

            if(mState==OK)
            {
                // Local Mapping might have changed some MapPoints tracked in last frame
                // lastframe中可以看到的mappoint替换为lastframe储存的备胎mappoint点mpReplaced，也就是更新mappoint
                // 更新一下mappoint，在回环的时候使用闭环关键帧的mappoint替换一些旧的mappoint
                CheckReplacedInLastFrame();

                // 运动模型是空的或刚完成重定位
                if(mVelocity.empty() || mCurrentFrame.mnId<mnLastRelocFrameId+2)
                {
                    /**
                    * 将参考帧关键帧的位姿作为当前帧的初始位姿进行跟踪；
                    * 匹配参考帧关键帧中有对应mappoint的特征点与当前帧特征点，通过dbow加速匹配；
                    * 优化3D点重投影误差，得到更精确的位姿以及剔除错误的特征点匹配；
                    * @return 如果匹配数大于10，返回true
                    */
                    bOK = TrackReferenceKeyFrame();//使用上一个参考关键帧的位姿作为初始值进行跟踪
                }
                else //运动模型不为空,且不是刚重定位,那么用运动模型跟踪
                {
                    bOK = TrackWithMotionModel();   //如果运动模型反馈的关键点正确匹配数<10
                    if(!bOK)
                        bOK = TrackReferenceKeyFrame();//则再进行一次使用上一个关键帧的位姿作为初始值进行跟踪
                }
            }
            else    //跟踪失败,进入重定位  (因为下面会根据bOK这个标志来设置 "mState")
            {
                //BOW搜索候选关键帧，PnP求解位姿
                bOK = Relocalization();
            }
        }
        else
        {
            // Localization Mode: Local Mapping is deactivated
            // 纯定位模式: 只进行跟踪tracking，局部地图不工作

            // tracking跟丢了
            if(mState==LOST)
            {
                //进入重定位
                bOK = Relocalization();
            }
            else
            {   //如果还没有跟丢,接下来检查跟踪是好还是快要凉

                // mbVO是mbOnlyTracking为true时的才有的一个变量
                // mbVO为false表示上一帧匹配了很多的MapPoints，跟踪很正常，
                // mbVO为true表明匹配了很少的MapPoints，少于10个，要跪的节奏
                if(!mbVO)
                {
                    // In last frame we tracked enough MapPoints in the map
                    // 上一帧跟踪到了足够多的路标点mappoint
                    if(!mVelocity.empty())
                    {
                        //如果恒速度模型不为空,则使用恒速度模型跟踪
                        bOK = TrackWithMotionModel();
                    }
                    else
                    {
                        //否则使用上一个关键帧参考帧来跟踪
                        bOK = TrackReferenceKeyFrame();
                    }
                }
                else
                {
                    // In last frame we tracked mainly "visual odometry" points.
                    // 上一帧只匹配到比较少的mappoint,要跪的节奏

                    // We compute two camera poses, one from motion model and one doing relocalization.
                    // If relocalization is sucessfull we choose that solution, otherwise we retain
                    // the "visual odometry" solution.

                    //我们跟踪和重定位都计算，如果重定位成功就选择重定位来计算位姿
                    bool bOKMM = false;
                    bool bOKReloc = false;
                    vector<MapPoint*> vpMPsMM;
                    vector<bool> vbOutMM;
                    cv::Mat TcwMM;
                    //如果运动模型非空
                    if(!mVelocity.empty())
                    {
                        //使用运动模型进行跟踪
                        bOKMM = TrackWithMotionModel();
                        vpMPsMM = mCurrentFrame.mvpMapPoints;
                        vbOutMM = mCurrentFrame.mvbOutlier;
                        TcwMM = mCurrentFrame.mTcw.clone();
                    }
                    //使用重定位计算位姿
                    bOKReloc = Relocalization();

                    //跟踪成功，重定位失败
                    if(bOKMM && !bOKReloc)
                    {
                        mCurrentFrame.SetPose(TcwMM);
                        mCurrentFrame.mvpMapPoints = vpMPsMM;
                        mCurrentFrame.mvbOutlier = vbOutMM;

                        //TrackWithMotionModel()函数会更新mbVO标志
                        if(mbVO)
                        {   //如果匹配数还是太少,

                            for(int i =0; i<mCurrentFrame.N; i++)
                            {
                                if(mCurrentFrame.mvpMapPoints[i] && !mCurrentFrame.mvbOutlier[i])
                                {
                                    mCurrentFrame.mvpMapPoints[i]->IncreaseFound();
                                }
                            }
                        }
                    }
                    //如果重定位成功
                    else if(bOKReloc)
                    {
                        //去除mbVO,表明匹配点数还比较多
                        mbVO = false;
                    }
                    //当跟踪模型跟踪失败\或者重定位也失败,则表明真的挂了
                    //只要有一个好的,bOK=True
                    bOK = bOKReloc || bOKMM;
                }
            }
        }
        ////////上面的跟踪得到了根据上一关键帧或者重定位得到的当前帧位姿估计(粗略估计)//////////

        //设置当前帧的参考关键帧，与当前帧共视的mappoint数量最多的关键帧
        mCurrentFrame.mpReferenceKF = mpReferenceKF;

        // If we have an initial estimation of the camera pose and matching. Track the local map.
        // 如果得到了相机位姿和匹配的初始估计,则跟踪局部地图
        if(!mbOnlyTracking)
        {
            // 如果不是纯跟踪模式
            if(bOK)
                bOK = TrackLocalMap();  //跟踪局部地图(最后一次优化)
        }
        else
        {
            // 如果是纯跟踪模式
            // mbVO true means that there are few matches to MapPoints in the map. We cannot retrieve
            // a local map and therefore we do not perform TrackLocalMap(). Once the system relocalizes
            // the camera we will use the local map again.
            if(bOK && !mbVO)
                bOK = TrackLocalMap();
        }

        if(bOK)
            mState = OK;
        else
            mState=LOST;

        // Update drawer
        mpFrameDrawer->Update(this);

        // If tracking were good, check if we insert a keyframe
        // 如果跟踪良好
        if(bOK)
        {
            // Update motion model
            // 更新运动模型
            if(!mLastFrame.mTcw.empty())
            {
                //更新恒速运动模型TrackWithMotionModel中的mVelocity
                //基于上一帧(注意不是上一个关键帧也不是参考关键帧)和当前帧的变换
                cv::Mat LastTwc = cv::Mat::eye(4,4,CV_32F);
                mLastFrame.GetRotationInverse().copyTo(LastTwc.rowRange(0,3).colRange(0,3));
                mLastFrame.GetCameraCenter().copyTo(LastTwc.rowRange(0,3).col(3));
                mVelocity = mCurrentFrame.mTcw*LastTwc;
                //mVelocity: 前后两帧的相对变换
            }
            else
                mVelocity = cv::Mat();

            // 设置当前相机位姿,用于可视化
            mpMapDrawer->SetCurrentCameraPose(mCurrentFrame.mTcw);

            // Clean VO matches
            // 清除UpdateLastFrame中为当前帧临时添加的MapPoints
            // ???
            for(int i=0; i<mCurrentFrame.N; i++)
            {
                MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];
                if(pMP)
                    if(pMP->Observations()<1)
                    {
                        mCurrentFrame.mvbOutlier[i] = false;
                        mCurrentFrame.mvpMapPoints[i]=static_cast<MapPoint*>(NULL);
                    }
            }

            // Delete temporal MapPoints（仅双目和rgbd）
            // 清除临时的MapPoints，这些MapPoints在TrackWithMotionModel的UpdateLastFrame函数里生成（仅双目和rgbd）
            // 这些生成的mappoint仅仅是为了提高双目或rgbd摄像头的帧间跟踪效果，用完以后就扔了，没有添加到地图中
            for(list<MapPoint*>::iterator lit = mlpTemporalPoints.begin(), lend =  mlpTemporalPoints.end(); lit!=lend; lit++)
            {
                MapPoint* pMP = *lit;
                delete pMP;
            }
            mlpTemporalPoints.clear();

            // Check if we need to insert a new keyframe
            // 判断是否插入keyframe
            if(NeedNewKeyFrame())
                CreateNewKeyFrame();

            // We allow points with high innovation (considererd outliers by the Huber Function)
            // pass to the new keyframe, so that bundle adjustment will finally decide
            // if they are outliers or not. We don't want next frame to estimate its position
            // with those points so we discard them in the frame.
            // 删除那些在bundle adjustment中检测为outlier的3D map点
            for(int i=0; i<mCurrentFrame.N;i++)
            {
                //删除当前帧在优化中被判断为outlier的路标点(3D点)
                if(mCurrentFrame.mvpMapPoints[i] && mCurrentFrame.mvbOutlier[i])
                    mCurrentFrame.mvpMapPoints[i]=static_cast<MapPoint*>(NULL);
            }
        }

        // Reset if the camera get lost soon after initialization
        // 如果mState==LOST，且mpMap->KeyFramesInMap()<=5（说明刚刚初始化），则reset；
        // 如果mState==LOST，且mpMap->KeyFramesInMap()>5，则会在下一帧中执行bOK = Relocalization();
        if(mState==LOST)
        {
            //如果跟踪失败
            if(mpMap->KeyFramesInMap()<=5)
            {
                // 并且才刚刚初始化完成
                cout << "Track lost soon after initialisation, reseting..." << endl;
                // 系统重置
                // 设置了一个System::mbReset标志位,其他线程根据这个标志位进行重置
                mpSystem->Reset();
                return;
            }
        }
        //如果当前帧还没设置参考关键帧
        if(!mCurrentFrame.mpReferenceKF)
            mCurrentFrame.mpReferenceKF = mpReferenceKF;    //设置参考关键帧(与mCurrentFrame共视点最多的关键帧)

        //准备下一帧的到来
        mLastFrame = Frame(mCurrentFrame);
    }

    // Store frame pose information to retrieve the complete camera trajectory afterwards.
    // 储存
    if(!mCurrentFrame.mTcw.empty())
    {
        //Tracking::mpReferenceKF : 与当前帧mCurrentFrame共视点数最多的关键帧,作为当前跟踪的参考关键帧
        //Tracking::mpReferenceKF : 在函数CreateNewKeyFrame()和UpdateLocalKeyFrames() 以及跟踪初始化的时候被设置
        //mCurrentFrame.mpReferenceKF : 与Tracking::mpReferenceKF保持一致
        //Tcr: 当前帧与参考关键帧的位姿相对变换
        cv::Mat Tcr = mCurrentFrame.mTcw*mCurrentFrame.mpReferenceKF->GetPoseInverse();
        //为每一帧储存对应的参考关键帧及其相对的变换
        mlRelativeFramePoses.push_back(Tcr);
        mlpReferences.push_back(mpReferenceKF);
        mlFrameTimes.push_back(mCurrentFrame.mTimeStamp);
        //跟踪状态储存
        mlbLost.push_back(mState==LOST);
    }
    else
    {
        // This can happen if tracking is lost
        // 跟踪失败才会进入到这里,即mCurrentFrame.mTcw为空
        mlRelativeFramePoses.push_back(mlRelativeFramePoses.back());
        mlpReferences.push_back(mlpReferences.back());
        mlFrameTimes.push_back(mlFrameTimes.back());
        mlbLost.push_back(mState==LOST);
    }

}

// 双目初始化
void Tracking::StereoInitialization()
{
    //特征点要大于500个才能初始化
    if(mCurrentFrame.N>500)
    {
        // Set Frame pose to the origin
        // 设置当前帧位姿
        mCurrentFrame.SetPose(cv::Mat::eye(4,4,CV_32F));

        // Create KeyFrame
        // 创建关键帧
        KeyFrame* pKFini = new KeyFrame(mCurrentFrame,mpMap,mpKeyFrameDB);

        // Insert KeyFrame in the map
        // 关键帧插入地图
        mpMap->AddKeyFrame(pKFini);

        // Create MapPoints and asscoiate to KeyFrame
        // 双目、RGBD创建mappoint
        for(int i=0; i<mCurrentFrame.N;i++) //遍历当前帧特征点
        {
            float z = mCurrentFrame.mvDepth[i];
            if(z>0)
            {
                // 将特征点根据当前帧位姿(上面设置了为世界坐标系原点)，恢复对应的3D点
                cv::Mat x3D = mCurrentFrame.UnprojectStereo(i);
                MapPoint* pNewMP = new MapPoint(x3D,pKFini,mpMap);
                // mappoint增加观测(表示可以被当前帧观测到)
                pNewMP->AddObservation(pKFini,i);
                // 当前帧加入可以观测到的mappoint
                pKFini->AddMapPoint(pNewMP,i);
                // 计算此mappoint能被看到的特征点中找出最能代表此mappoint的描述子
                pNewMP->ComputeDistinctiveDescriptors();
                // 更新此mappoint参考帧光心到mappoint平均观测方向以及观测距离范围
                pNewMP->UpdateNormalAndDepth();
                // 地图插入mappoint
                mpMap->AddMapPoint(pNewMP);
                // 设置当前帧第i个特征点的mappoint
                mCurrentFrame.mvpMapPoints[i]=pNewMP;
            }
        }

        cout << "New map created with " << mpMap->MapPointsInMap() << " points" << endl;

        //LocalMapping插入关键帧
        mpLocalMapper->InsertKeyFrame(pKFini);

        //当前帧传保存为LastFrame
        mLastFrame = Frame(mCurrentFrame);
        mnLastKeyFrameId=mCurrentFrame.mnId;
        mpLastKeyFrame = pKFini;

        //储存当前帧
        mvpLocalKeyFrames.push_back(pKFini);
        //设置局部地图mappoint
        mvpLocalMapPoints=mpMap->GetAllMapPoints();
        mpReferenceKF = pKFini;
        mCurrentFrame.mpReferenceKF = pKFini;

        // 将局部地图mappoint设置为参考mappoint，用于绘图
        mpMap->SetReferenceMapPoints(mvpLocalMapPoints);
        //按顺序储存关键帧到地图
        mpMap->mvpKeyFrameOrigins.push_back(pKFini);
        //设置绘图器相机位姿为当前帧位姿(用于可视化)
        mpMapDrawer->SetCurrentCameraPose(mCurrentFrame.mTcw);

        mState=OK;
    }
}

//单目初始化
void Tracking::MonocularInitialization()
{

    // 如果单目初始器还没有被创建，则创建单目初始器
    if(!mpInitializer)
    {
        // Set Reference Frame
        //如果如果当前帧特征点数量大于100
        if(mCurrentFrame.mvKeys.size()>100)
        {
            //设置mInitialFrame，和mLastFrame
            mInitialFrame = Frame(mCurrentFrame);
            mLastFrame = Frame(mCurrentFrame);
            // mvbPrevMatched最大的情况就是所有特征点都被跟踪上
            mvbPrevMatched.resize(mCurrentFrame.mvKeysUn.size());
            for(size_t i=0; i<mCurrentFrame.mvKeysUn.size(); i++)
                mvbPrevMatched[i]=mCurrentFrame.mvKeysUn[i].pt;

            //确认mpInitializer指向NULL
            if(mpInitializer)
                delete mpInitializer;

            mpInitializer =  new Initializer(mCurrentFrame,1.0,200);
	    
            //将mvIniMatches全部初始化为-1
            //初始化时得到的特征点匹配，大小是mInitialFrame的特征点数量，其值是当前帧特征点序号
            fill(mvIniMatches.begin(),mvIniMatches.end(),-1);

            return;
        }
    }
    //如果指针mpInitializer没有指向NULL，也就是说我们在之前已经通过mInitialFrame新建过一个Initializer对象，并使 mpInitializer指向它了
    else
    {
        // Try to initialize
        //尝试初始化
        //如果当前帧特征点数量<=100
        if((int)mCurrentFrame.mvKeys.size()<=100)
        {
            //删除mpInitializer指针并指向NULL
            delete mpInitializer;
            mpInitializer = static_cast<Initializer*>(NULL);
            fill(mvIniMatches.begin(),mvIniMatches.end(),-1);
            return;
        }

        // Find correspondences
        //新建一个ORBmatcher对象
        ORBmatcher matcher(0.9,true);
        //寻找特征点匹配
        int nmatches = matcher.SearchForInitialization(mInitialFrame,mCurrentFrame,mvbPrevMatched,mvIniMatches,100);

        // Check if there are enough correspondences
        //如果匹配的点过少，则删除当前的初始化器
        if(nmatches<100)
        {
            delete mpInitializer;
            mpInitializer = static_cast<Initializer*>(NULL);
            return;
        }
        //到这里说明初始化条件已经成熟

        //开始要计算位姿R，t了！！
        cv::Mat Rcw; // Current Camera Rotation
        cv::Mat tcw; // Current Camera Translation
        //初始化成功后，匹配点中三角化投影成功的情况
        vector<bool> vbTriangulated; // Triangulated Correspondences (mvIniMatches)

        // 通过H模型或F模型进行单目初始化，得到两帧间相对运动、初始MapPoints
        if(mpInitializer->Initialize(mCurrentFrame, mvIniMatches, Rcw, tcw, mvIniP3D, vbTriangulated))
        {
            //ReconstructH，或者ReconstructF中解出RT后，会有一些点不能三角化重投影成功。
            //在根据vbTriangulated中特征点三角化投影成功的情况，去除一些匹配点
            for(size_t i=0, iend=mvIniMatches.size(); i<iend;i++)
            {
                if(mvIniMatches[i]>=0 && !vbTriangulated[i])
                {
                    mvIniMatches[i]=-1;
                    nmatches--;
                }
            }

            // Set Frame Poses
            //初始化mInitialFrame的位姿
            //将mInitialFrame位姿设置为世界坐标
            mInitialFrame.SetPose(cv::Mat::eye(4,4,CV_32F));
            cv::Mat Tcw = cv::Mat::eye(4,4,CV_32F);
            Rcw.copyTo(Tcw.rowRange(0,3).colRange(0,3));
            tcw.copyTo(Tcw.rowRange(0,3).col(3));
            //将mpInitializer->Initialize算出的R和t拷贝到当前帧mCurrentFrame的位姿
            mCurrentFrame.SetPose(Tcw);

            //创建单目的初始化地图
            CreateInitialMapMonocular();
        }
    }
}

void Tracking::CreateInitialMapMonocular()
{
    // Create KeyFrames
  
    KeyFrame* pKFini = new KeyFrame(mInitialFrame,mpMap,mpKeyFrameDB);
    KeyFrame* pKFcur = new KeyFrame(mCurrentFrame,mpMap,mpKeyFrameDB);


    //计算关键帧的词袋bow和featurevector
    pKFini->ComputeBoW();
    pKFcur->ComputeBoW();

    // Insert KFs in the map
    mpMap->AddKeyFrame(pKFini);
    mpMap->AddKeyFrame(pKFcur);

    // Create MapPoints and asscoiate to keyframes
    //遍历每一个初始化时得到的特征点匹配,将三角化重投影成功的特征点转化为mappoint(地图点/路标点)
    for(size_t i=0; i<mvIniMatches.size();i++)
    {
        if(mvIniMatches[i]<0)
            continue;

        //Create MapPoint.
        cv::Mat worldPos(mvIniP3D[i]);

         //新建mappoint对象，注意这些mappoint是pKFcur能够看到的
        MapPoint* pMP = new MapPoint(worldPos,pKFcur,mpMap);

        //给关键帧添加mappoint，让keyframe知道自己可以看到哪些mappoint
        pKFini->AddMapPoint(pMP,i);     //i ： mappoint对应的在关键帧上的特征点
        pKFcur->AddMapPoint(pMP,mvIniMatches[i]);
	
        //让pMP知道自己可以被pKFini，pKFcur看到
        pMP->AddObservation(pKFini,i);
        pMP->AddObservation(pKFcur,mvIniMatches[i]);

        //找出最能代表此mappoint的描述子
        pMP->ComputeDistinctiveDescriptors();
        //更新此mappoint参考帧光心到mappoint平均观测方向以及观测距离范围
        pMP->UpdateNormalAndDepth();

        //Fill Current Frame structure
        //更新当前帧Frame能看到哪些mappoint
        mCurrentFrame.mvpMapPoints[mvIniMatches[i]] = pMP;
        //标记当前帧的特征点不是是Outlier
        mCurrentFrame.mvbOutlier[mvIniMatches[i]] = false;

        //Add to Map
        //向map添加mappoint
        mpMap->AddMapPoint(pMP);
    }

    // Update Connections
    //更新共视图Covisibility graph,essential graph和spanningtree
    pKFini->UpdateConnections();
    pKFcur->UpdateConnections();

    // Bundle Adjustment
    cout << "New Map created with " << mpMap->MapPointsInMap() << " points" << endl;

    //进行一次全局BA
    Optimizer::GlobalBundleAdjustemnt(mpMap,20);

    // Set median depth to 1

    // 将MapPoints的中值深度归一化到1，并归一化两帧之间变换
    // 评估关键帧场景深度，q=2表示中值
    float medianDepth = pKFini->ComputeSceneMedianDepth(2); //返回mappoint集合在初始化帧的深度的中位数
    float invMedianDepth = 1.0f/medianDepth;

    // 如果深度中位数<0 ， 或者 本关键帧观测到的mappoint被其他关键帧观测的次数太少了
    if(medianDepth<0 || pKFcur->TrackedMapPoints(1)<100)
    {
        cout << "Wrong initialization, reseting..." << endl;
        Reset();
        return;
    }

    // Scale initial baseline
    cv::Mat Tc2w = pKFcur->GetPose();   //取当前帧位姿(世界坐标系到相机坐标系的变换)
    // 利用invMedianDepth归一化
    // 使用逆深度中位数归一化Tcw的平移部分
    Tc2w.col(3).rowRange(0,3) = Tc2w.col(3).rowRange(0,3)*invMedianDepth;
    pKFcur->SetPose(Tc2w);

    // Scale points
    // 把3D点的尺度也归一化到1
    vector<MapPoint*> vpAllMapPoints = pKFini->GetMapPointMatches();   //取本关键帧观测到的mappoint
    for(size_t iMP=0; iMP<vpAllMapPoints.size(); iMP++)
    {
        if(vpAllMapPoints[iMP])
        {
            MapPoint* pMP = vpAllMapPoints[iMP];
            //使用逆深度中位数对3D点坐标进行归一化
            pMP->SetWorldPos(pMP->GetWorldPos()*invMedianDepth);
        }
    }

    //向mpLocalMapper插入关键帧pKFini，pKFcur
    mpLocalMapper->InsertKeyFrame(pKFini);
    mpLocalMapper->InsertKeyFrame(pKFcur);

    mCurrentFrame.SetPose(pKFcur->GetPose());
    mnLastKeyFrameId=mCurrentFrame.mnId;
    mpLastKeyFrame = pKFcur;

    //局部地图
    mvpLocalKeyFrames.push_back(pKFcur);
    mvpLocalKeyFrames.push_back(pKFini);
    mvpLocalMapPoints=mpMap->GetAllMapPoints();
    //为下一帧做准备
    mpReferenceKF = pKFcur;
    mCurrentFrame.mpReferenceKF = pKFcur;

    //上一帧设置为mCurrentFrame
    mLastFrame = Frame(mCurrentFrame);

    // 将局部地图mappoint设置为参考mappoint，用于绘图
    mpMap->SetReferenceMapPoints(mvpLocalMapPoints);

    //地图绘制器
    //设置相机位姿为当前关键帧位姿
    mpMapDrawer->SetCurrentCameraPose(pKFcur->GetPose());

    //按顺序储存关键帧到地图
    mpMap->mvpKeyFrameOrigins.push_back(pKFini);

    mState=OK;
}

void Tracking::CheckReplacedInLastFrame()
{
    //遍历上一帧所有特征点
    for(int i =0; i<mLastFrame.N; i++)
    {
        //取上一帧观测到的mappoint
        MapPoint* pMP = mLastFrame.mvpMapPoints[i];

        if(pMP)
        {
            MapPoint* pRep = pMP->GetReplaced();
            if(pRep)
            {
                mLastFrame.mvpMapPoints[i] = pRep;
            }
        }
    }
}

//跟踪,计算当前帧前端优化位姿
bool Tracking::TrackReferenceKeyFrame()
{
    // Compute Bag of Words vector
    //计算当前帧的Bow向量
    mCurrentFrame.ComputeBoW();

    // We perform first an ORB matching with the reference keyframe
    // If enough matches are found we setup a PnP solver
    ORBmatcher matcher(0.7,true);
    vector<MapPoint*> vpMapPointMatches;

    //通过特征点的BoW加速匹配当前帧与参考关键帧之间的特征点
    //这个函数会设置vpMapPointMatches[]
    //vpMapPointMatches[当前帧特征点idx]=根据参考关键帧进行匹配得到的mappoint
    //返回匹配数
    int nmatches = matcher.SearchByBoW(mpReferenceKF,mCurrentFrame,vpMapPointMatches);

    //匹配数小于15，表示跟踪失败
    if(nmatches<15)
        return false;

    //匹配点对更新
    //这个操作将会把当前帧特征点和对应参考关键帧的特征点--3D点建立连接,在下面的PoseOptimization()的BA优化中使用
    mCurrentFrame.mvpMapPoints = vpMapPointMatches;

    //将上一帧的位姿作为当前帧位姿的初始值，在PoseOptimization可以收敛快一些
    //这一个操作对与下一步的PoseOptimization()有用
    mCurrentFrame.SetPose(mLastFrame.mTcw);
    
    //通过优化3D-2D的重投影误差来获得位姿
    //使用g2o来解Pnp问题
    Optimizer::PoseOptimization(&mCurrentFrame);//这个过程会返回当前帧的优化位姿,以及某个特征点是否outlier的标志

    int nmatchesMap = 0;//所有关键点有效匹配数
    //剔除优化后的outlier匹配点
    //遍历mCurrentFrame每个特征点
    for(int i =0; i<mCurrentFrame.N; i++)
    {
        //如果这个特征点有相对应的mappoint
        if(mCurrentFrame.mvpMapPoints[i]) //判断当前索引的特征点是否有世界坐标点(地图点),若当前索引没有对应点则NULL
        {
            //如果这个mappoint在上次优化中被标记为outlier，则剔除
            if(mCurrentFrame.mvbOutlier[i]) //若当前对应的世界坐标点(地图点\路标点)在优化的时候被标记为outilie,则剔除
            {
                MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];

                mCurrentFrame.mvpMapPoints[i]=static_cast<MapPoint*>(NULL);//剔除
                mCurrentFrame.mvbOutlier[i]=false;//恢复标志位
                pMP->mbTrackInView = false;         //被标记为outlier的地图点(路标点)跟踪标志位设置为false,不再跟踪
                pMP->mnLastFrameSeen = mCurrentFrame.mnId;//设置这个outlier地图点(路标点)上一次跟踪的frame_id为当前帧
                nmatches--; //特征点匹配数-1
            }
            else if(mCurrentFrame.mvpMapPoints[i]->Observations()>0)
                //否则,如果这个地图点被观测到的帧数>0,那么当前帧所有关键点有效匹配数+1
                nmatchesMap++;
        }
    }

    return nmatchesMap>=10; //检查所有关键点有效匹配数>10?
}

void Tracking::UpdateLastFrame()
{
    // Update pose according to reference keyframe
    //根据上一帧的
    KeyFrame* pRef = mLastFrame.mpReferenceKF;//上一帧所对应的参考关键帧,（共视的mappoint数量最多）的关键帧,如果上一帧就是关键帧,那么参考关键帧就是它本身
    cv::Mat Tlr = mlRelativeFramePoses.back();//取最接近的对应的参考关键帧及其相对的变换

    //上一帧相对世界坐标系的位姿利用上一帧的参考关键帧的位姿和相对变换计算
    //所以上一帧的位姿= T_{lastFrame<--lastKeyF} * Pose_{lastKeyF} = T_(last<--refKF) * T_(refKF<--world)
    mLastFrame.SetPose(Tlr*pRef->GetPose());

    //如果上一帧就是对应的关键帧,或者是单目相机,又或者是正常的slam,则return
    if(mnLastKeyFrameId==mLastFrame.mnId || mSensor==System::MONOCULAR || !mbOnlyTracking)
        return;

    // Create "visual odometry" MapPoints
    // We sort points according to their measured depth by the stereo/RGB-D sensor
    // 通过双目或者RGB-D相机对测量的深度进行排序
    // 将mLastFrame深度z>0筛选出来放入vDepthIdx
    vector<pair<float,int> > vDepthIdx;
    vDepthIdx.reserve(mLastFrame.N);
    for(int i=0; i<mLastFrame.N;i++)
    {
        float z = mLastFrame.mvDepth[i];
        if(z>0)
        {
            vDepthIdx.push_back(make_pair(z,i));
        }
    }

    if(vDepthIdx.empty())
        return;
    //按照深度从小到大排序
    sort(vDepthIdx.begin(),vDepthIdx.end());

    // We insert all close points (depth<mThDepth)
    // If less than 100 close points, we insert the 100 closest ones.
    // 插入所有比较近的点, mThDepth是距离阈值,通常认为距离相机比较近的点更加可靠?
    // 遍历vDepthIdx
    int nPoints = 0;
    for(size_t j=0; j<vDepthIdx.size();j++)
    {
        int i = vDepthIdx[j].second;    // 深度大于0的对应的索引 , 在(mLastFrame.mvDepth)的索引

        bool bCreateNew = false;

        MapPoint* pMP = mLastFrame.mvpMapPoints[i]; //取对应索引的世界坐标点(地图点)
    //如果mLastFrame的第i个特征点没有对应的世界坐标点(地图点)
        if(!pMP)
            bCreateNew = true;  //设置标志位,表示需要创建
        else if(pMP->Observations()<1)  //或者mLastFrame的第i个特征点对应的世界坐标点被观测的帧数<1,相当于没有被观测到
        {
            bCreateNew = true;  //也需要设置标志位,需要创建
        }

        //若当前索引没有对应的世界坐标点(地图点),则创建一个
        if(bCreateNew)  //标志位
        {
            cv::Mat x3D = mLastFrame.UnprojectStereo(i);    //创建索引为i的关键点对应的世界坐标系点(路标点)
            MapPoint* pNewMP = new MapPoint(x3D,mpMap,&mLastFrame,i);   //利用上面的路标点创建地图点,参考帧为上一帧,

            mLastFrame.mvpMapPoints[i]=pNewMP;  //设置: 上一帧的地图点为上面创建的pNewMP

            mlpTemporalPoints.push_back(pNewMP);// 推入?
            nPoints++;  //根据上一帧的路标点总数+1
        }
        else
        {
            nPoints++;
        }

        //vDepthIdx是从小到达排序的深度
        //如果最小值也大于阈值,或者上一帧的路标点总数>100,则直接跳出
        if(vDepthIdx[j].first>mThDepth && nPoints>100)
            break;
    }
}

bool Tracking::TrackWithMotionModel()
{
    //0.9:最好匹配与次好匹配差距的阈值。其值越小，其匹配越精确
    ORBmatcher matcher(0.9,true);

    // Update last frame pose according to its reference keyframe
    // Create "visual odometry" points if in Localization Mode

    // 定位模式下,或者对于双目或rgbd摄像头，将执行:
    // 根据深度值为上一关键帧生成新的MapPoints
    //（跟踪过程中需要将当前帧与上一帧进行特征点匹配，将上一帧的MapPoints投影到当前帧可以缩小匹配范围）
    // 在前端优化的过程中，会去除outlier的MapPoint，如果不及时增加MapPoint会逐渐减少
    // 这个函数的功能就是补充增加RGBD和双目相机上一帧的MapPoints数
    UpdateLastFrame();

    
    // 根据Const Velocity Model(认为这两帧之间的相对运动和之前两帧间相对运动相同)估计当前帧的粗略位姿
    // 恒速度模型
    mCurrentFrame.SetPose(mVelocity*mLastFrame.mTcw);   //mVelocity在哪里被设置?

    fill(mCurrentFrame.mvpMapPoints.begin(),mCurrentFrame.mvpMapPoints.end(),static_cast<MapPoint*>(NULL));

    // Project points seen in previous frame
    int th;
    if(mSensor!=System::STEREO)
        th=15;
    else
        th=7;
    
    /// 根据上一帧LastFrame的特征点以及所对应的mappoint信息，寻找当前帧的哪些特征点与哪些mappoint的匹配联系,
    /// (也就是求出当前帧观测到的并且上一帧也观测到的路标点)
    // 函数会根据上一帧特征点对应的3D点投影的位置缩小特征点匹配范围
    // SearchByProjection()函数会设置mCurrentFrame.mvpMapPoints[]这个数组,
    // mCurrentFrame.mvpMapPoints[特征点idx]=对应的路标(3D)点
    // mvpMapPoints[]:描述与当前帧特征点对应的3D点
    int nmatches = matcher.SearchByProjection(mCurrentFrame,mLastFrame,th,mSensor==System::MONOCULAR);

    // If few matches, uses a wider window search
    //匹配数量太少，扩大特征匹配搜索框重新进行mappoint跟踪
    if(nmatches<20)
    {
        fill(mCurrentFrame.mvpMapPoints.begin(),mCurrentFrame.mvpMapPoints.end(),static_cast<MapPoint*>(NULL));
        nmatches = matcher.SearchByProjection(mCurrentFrame,mLastFrame,2*th,mSensor==System::MONOCULAR);
    }

    if(nmatches<20)
        return false;

    // Optimize frame pose with all matches
    // 当前帧观测到的路标点,以及特征点,进行重投影误差求解Pnp
    Optimizer::PoseOptimization(&mCurrentFrame);


    int nmatchesMap = 0;
    // Discard outliers
    //上一步的位姿优化更新了mCurrentFrame的outlier[]，需要将mCurrentFrame的mvpMapPoints更新
    for(int i =0; i<mCurrentFrame.N; i++)
    {
        if(mCurrentFrame.mvpMapPoints[i])
        {
            //如果在优化的时候,这个索引对应的路标点被标记为外点,则剔除
            if(mCurrentFrame.mvbOutlier[i])
            {
                MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];

                mCurrentFrame.mvpMapPoints[i]=static_cast<MapPoint*>(NULL);
                mCurrentFrame.mvbOutlier[i]=false;
                pMP->mbTrackInView = false;
                pMP->mnLastFrameSeen = mCurrentFrame.mnId;
                nmatches--;
            }
            //否则,如果当前帧可以看到的mappoint同时能被其他keyframe看到
            else if(mCurrentFrame.mvpMapPoints[i]->Observations()>0)
                nmatchesMap++;
        }
    }    

    //如果只跟踪,那么则判断与上一帧匹配上的特征点的数量是否大于阈值
    if(mbOnlyTracking)
    {
        //mbVO: 通过匹配点数来判断跟踪情况, mbVO=true:表示匹配数少,跟踪准备挂
        mbVO = nmatchesMap<10;
        return nmatches>20;
    }
    //如果优化完之后的
    return nmatchesMap>=10;
}

bool Tracking::TrackLocalMap()
{
    // We have an estimation of the camera pose and some map points tracked in the frame.
    // We retrieve the local map and try to find matches to points in the local map.
    // 如果前面的步骤基于上一个关键帧或者重定位得到了当前帧的大概位姿和一些路标点
    // 接下来获取局部地图,尝试匹配当前帧与局部地图的mappoint

    // 更新局部地图，即更新局部地图关键帧，局部地图mappoint
    // 利用与当前帧有关系的一些关键帧来构建一个局部地图
    UpdateLocalMap();

    // 在局部地图的mappoint中查找在当前帧视野范围内的点，将视野范围内的点和当前帧的特征点进行投影匹配
    SearchLocalPoints();

    // 上面的主要作用是,构建局部地图,
    // 基于局部地图,为当前帧继续增加2D-3D点匹配,用于下面的BA优化
    ////////////////////////////////////////////////////////////////////////////

    // Relocalization、TrackReferenceKeyFrame、TrackWithMotionModel中都有前端BA位姿优化 :
    // 1. Relocalization(): 基于所有的关键帧,先通过BOW搜索候选关键帧,才从候选关键帧一个个筛选
    // 2. TrackReferenceKeyFrame(): 基于参考关键帧与当前帧,为当前帧寻找3D-2D点匹配,进行BA
    // 3. TrackWithMotionModelI(): 基于上一帧和当前帧以及运动模型,为当前帧寻找3D-2D点匹配,进行BA

    // 4. 这里是基于局部地图,从局部地图里面寻找当前帧匹配的3D-2D点匹配,再进行位姿
    // Optimize Pose
    Optimizer::PoseOptimization(&mCurrentFrame);
    mnMatchesInliers = 0;

    // Update MapPoints Statistics
    // 更新当前帧的MapPoints被观测程度，并统计跟踪局部地图的效果
    for(int i=0; i<mCurrentFrame.N; i++)
    {
        if(mCurrentFrame.mvpMapPoints[i])
        {
            //如果在优化中没有被标记为外点
            if(!mCurrentFrame.mvbOutlier[i])
            {
                //标记该mappoint点被当前帧观测
                mCurrentFrame.mvpMapPoints[i]->IncreaseFound();
                if(!mbOnlyTracking)
                {
                    if(mCurrentFrame.mvpMapPoints[i]->Observations()>0)
                        mnMatchesInliers++;
                }
                else
                    mnMatchesInliers++;
            }
            else if(mSensor==System::STEREO)    //如果是外点,而且是双目,则剔除这个mappoint与当前帧的联系
                mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint*>(NULL);

        }
    }

    // Decide if the tracking was succesful
    // More restrictive if there was a relocalization recently
    // 决定是否跟踪成功
    // 如果当前帧和上一次重定位太近并且当前帧特征点与mappoint的匹配数太少(<50)
    if(mCurrentFrame.mnId<mnLastRelocFrameId+mMaxFrames && mnMatchesInliers<50)
        return false;

    // 或者当前帧特征点与mappoint的匹配数太少(但是不是重定位不久)
    if(mnMatchesInliers<30)
        return false;
    else
        return true;
}


bool Tracking::NeedNewKeyFrame()
{
    //如果只是跟踪定位，则不插入关键帧
    if(mbOnlyTracking)
        return false;

    // If Local Mapping is freezed by a Loop Closure do not insert keyframes
    // 如果地图被回环线程锁定了或者建图器已经关闭，则不插入关键帧
    if(mpLocalMapper->isStopped() || mpLocalMapper->stopRequested())
        return false;

    //获取当前地图上的关键帧数量
    const int nKFs = mpMap->KeyFramesInMap();

    // Do not insert keyframes if not enough frames have passed from last relocalisation
    // 如果刚刚重定位完并且地图上的关键帧>阈值,也不插入关键帧
    if(mCurrentFrame.mnId<mnLastRelocFrameId+mMaxFrames && nKFs>mMaxFrames)
        return false;

    // Tracked MapPoints in the reference keyframe
    // 检查当前参考关键帧的mappoint有多少是被观测到3次以上的
    int nMinObs = 3;
    if(nKFs<=2)
        nMinObs=2;
    int nRefMatches = mpReferenceKF->TrackedMapPoints(nMinObs); //返回参考关键帧可以观测到的mappoint里面,被观测到minObs次以上的数量

    // Local Mapping accept keyframes?
    // 查询局部地图管理器是否繁忙
    bool bLocalMappingIdle = mpLocalMapper->AcceptKeyFrames();

    // Check how many "close" points are being tracked and how many could be potentially created.
    // 检查当前帧观测到的良好的mappoint的数量
    int nNonTrackedClose = 0;
    int nTrackedClose= 0;
    if(mSensor!=System::MONOCULAR)  //如果是双目或者RGB-D
    {
        for(int i =0; i<mCurrentFrame.N; i++)
        {
            if(mCurrentFrame.mvDepth[i]>0 && mCurrentFrame.mvDepth[i]<mThDepth)
            {
                if(mCurrentFrame.mvpMapPoints[i] && !mCurrentFrame.mvbOutlier[i])
                    nTrackedClose++;
                else
                    nNonTrackedClose++;
            }
        }
    }
    //如果良好跟踪的mappoint太少,以及跟踪不到的点过多,表明需要插入关键帧
    bool bNeedToInsertClose = (nTrackedClose<100) && (nNonTrackedClose>70);

    // Thresholds
    // 设定inlier阈值，和之前帧特征点匹配的inlier比例
    float thRefRatio = 0.75f;
    if(nKFs<2)
        thRefRatio = 0.4f;

    if(mSensor==System::MONOCULAR)
        thRefRatio = 0.9f;

    // Condition 1a: More than "MaxFrames" have passed from last keyframe insertion
    // 和上一个关键帧间隔需要大于mMaxFrames
    const bool c1a = mCurrentFrame.mnId>=mnLastKeyFrameId+mMaxFrames;
    // Condition 1b: More than "MinFrames" have passed and Local Mapping is idle
    // 如果Local Mapping空闲，且和上一个关键帧间隔需要大于mMinFrames
    const bool c1b = (mCurrentFrame.mnId>=mnLastKeyFrameId+mMinFrames && bLocalMappingIdle);
    // Condition 1c: tracking is weak
    // 如果不是单目, 内点数少或者跟踪不好,则需要插入关键帧
    const bool c1c =  mSensor!=System::MONOCULAR && (mnMatchesInliers<nRefMatches*0.25 || bNeedToInsertClose) ;
    // Condition 2: Few tracked points compared to reference keyframe. Lots of visual odometry compared to map matches.
    // 与当前参考关键帧被多次观测的mappoint数量相比,如果当前帧和局部地图匹配的内点太少 或者 跟踪不好
    // mnMatchesInliers: 当前帧与局部地图匹配的内点数
    const bool c2 = ((mnMatchesInliers<nRefMatches*thRefRatio|| bNeedToInsertClose) && mnMatchesInliers>15);

    // 如果需要插入关键帧
    if((c1a||c1b||c1c)&&c2)
    {
        // If the mapping accepts keyframes, insert keyframe.
        // Otherwise send a signal to interrupt BA
        // 如果地图可以接受新的关键帧,则插入
        if(bLocalMappingIdle)
        {
            return true;
        }
        else
        {   //如果地图在进行BA,则发出信号停止BA,
            //如果是双目或者RGB-D,那么可以把当前帧加入到队列

            // 局部地图器, 停止BA
            mpLocalMapper->InterruptBA();
            // 如果是双目或者RBG-D
            if(mSensor!=System::MONOCULAR)
            {
                // 关键帧队列还没满,则表示可以插入
                if(mpLocalMapper->KeyframesInQueue()<3)
                    return true;
                else
                    return false;
            }
            else
                return false;
        }
    }
    else
        return false;
}

void Tracking::CreateNewKeyFrame()
{
    if(!mpLocalMapper->SetNotStop(true))
        return;

    // 步骤1：将当前帧构造成关键帧
    KeyFrame* pKF = new KeyFrame(mCurrentFrame,mpMap,mpKeyFrameDB); //mpKeyFrameDB:跟踪所用的词袋数据库

    // 步骤2：跟踪器的参考关键帧也设置为当前帧, 当前帧的参考关键帧也设置为当前帧
    mpReferenceKF = pKF;    //为下一帧做准备
    mCurrentFrame.mpReferenceKF = pKF;

    // 这段代码和UpdateLastFrame中的那一部分代码功能相同
    // 步骤3：对于双目或rgbd摄像头，为当前帧生成新的MapPoints
    if(mSensor!=System::MONOCULAR)
    {
	// 根据Tcw计算mRcw、mtcw和mRwc、mOw
        mCurrentFrame.UpdatePoseMatrices();

        // We sort points by the measured depth by the stereo/RGBD sensor.
        // We create all those MapPoints whose depth < mThDepth.
        // If there are less than 100 close points we create the 100 closest.
	// 步骤3.1：得到当前帧深度小于阈值的特征点
        // 创建新的MapPoint, depth < mThDepth
        vector<pair<float,int> > vDepthIdx;
        vDepthIdx.reserve(mCurrentFrame.N);
        for(int i=0; i<mCurrentFrame.N; i++)
        {
            float z = mCurrentFrame.mvDepth[i];
            if(z>0)
            {
                vDepthIdx.push_back(make_pair(z,i));
            }
        }

        if(!vDepthIdx.empty())
        {
	    // 步骤3.2：按照深度从小到大排序
            sort(vDepthIdx.begin(),vDepthIdx.end());

	    // 步骤3.3：将距离比较近的点包装成MapPoints
            int nPoints = 0;
            for(size_t j=0; j<vDepthIdx.size();j++)
            {
                int i = vDepthIdx[j].second;

                bool bCreateNew = false;

                MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];
                if(!pMP)
                    bCreateNew = true;
                else if(pMP->Observations()<1)
                {
                    bCreateNew = true;
                    mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint*>(NULL);
                }

                if(bCreateNew)
                {
                    cv::Mat x3D = mCurrentFrame.UnprojectStereo(i);
                    MapPoint* pNewMP = new MapPoint(x3D,pKF,mpMap);
		    // 这些添加属性的操作是每次创建MapPoint后都要做的
                    pNewMP->AddObservation(pKF,i);
                    pKF->AddMapPoint(pNewMP,i);
                    pNewMP->ComputeDistinctiveDescriptors();
                    pNewMP->UpdateNormalAndDepth();
		    //向map中添加mappoint
                    mpMap->AddMapPoint(pNewMP);

                    mCurrentFrame.mvpMapPoints[i]=pNewMP;
                    nPoints++;
                }
                else
                {
                    nPoints++;
                }

                //如果深度大于阈值mThDepth，或者nPoints>100则停止添加mappoint点
                if(vDepthIdx[j].first>mThDepth && nPoints>100)
                    break;
            }
        }
    }
    //将当前帧这个关键帧push到局部地图新关键帧队列mlNewKeyFrames
    mpLocalMapper->InsertKeyFrame(pKF);

    mpLocalMapper->SetNotStop(false);
    //将跟踪器上一个关键帧设置为当前关键帧,为下一帧做准备
    mnLastKeyFrameId = mCurrentFrame.mnId;
    mpLastKeyFrame = pKF;
}

void Tracking::SearchLocalPoints()
{
    // Do not search map points already matched
    // 当前帧mCurrentFrame匹配的mappoint点就不要匹配了
    //这些匹配点都是在```TrackWithMotionModel()```，```TrackReferenceKeyFrame()```，```Relocalization()```中当前帧和mappoint的匹配
    for(vector<MapPoint*>::iterator vit=mCurrentFrame.mvpMapPoints.begin(), vend=mCurrentFrame.mvpMapPoints.end(); vit!=vend; vit++)
    {
        MapPoint* pMP = *vit;
        if(pMP)
        {
            if(pMP->isBad())
            {
                *vit = static_cast<MapPoint*>(NULL);
            }
            else
            {
                // 预测这个mappoint会被匹配(这个在LocalMapping.cc里面会用到)
                pMP->IncreaseVisible();
                // 记录该mappoint上一次被观测是当前帧
                pMP->mnLastFrameSeen = mCurrentFrame.mnId;
                // 标记该点将来在下面的matcher.SearchByProjection()不被投影，因为已经匹配过
                pMP->mbTrackInView = false;
            }
        }
    }

    int nToMatch=0;

    // Project points in frame and check its visibility
    // mvpLocalMapPoints在函数Tracking::UpdateLocalMap()里面被更新
    // 遍历刚才更新的局部地图mappoint，筛选哪些不在视野范围内的mappoint
    // 在视野范围内的mappoint是被预测我们能够和当前帧匹配上的mappoint点
    for(vector<MapPoint*>::iterator vit=mvpLocalMapPoints.begin(), vend=mvpLocalMapPoints.end(); vit!=vend; vit++)
    {
        MapPoint* pMP = *vit;
        //如果这个局部地图点是与当前帧相关联的(或者说当前帧能看到的),就跳过
        if(pMP->mnLastFrameSeen == mCurrentFrame.mnId)
            continue;
        if(pMP->isBad())
            continue;

        //Project (this fills MapPoint variables for matching)
        //如果此mappoint点在视野范围内
        if(mCurrentFrame.isInFrustum(pMP,0.5))
        {
            // 预测这个mappoint会被匹配(这个在LocalMapping.cc里面会用到)
            pMP->IncreaseVisible();
            nToMatch++;
        }
    }

    //当前帧和局部地图点进行匹配
    if(nToMatch>0)
    {
        ORBmatcher matcher(0.8);
        int th = 1;
        if(mSensor==System::RGBD)
            th=3;
        // If the camera has been relocalised recently, perform a coarser search
        if(mCurrentFrame.mnId<mnLastRelocFrameId+2) //如果才进行完重定位不久,搜索范围大一点
            th=5;
        //局部地图点和当前帧匹配(再次匹配,为了增加匹配对数,用于位姿优化)
        //如果某个局部地图点已经和当前帧匹配上了,则跳过该局部地图点
        matcher.SearchByProjection(mCurrentFrame,mvpLocalMapPoints,th);
    }
}

// 利用与当前帧有关系的一些关键帧来构建一个局部地图
void Tracking::UpdateLocalMap()
{
    // This is for visualization
    // mvpLocalKeyFrames的所有关键帧的所有匹配的mappoint集合
    // 用于可视化
    mpMap->SetReferenceMapPoints(mvpLocalMapPoints);

    // Update
    UpdateLocalKeyFrames(); //更新局部地图关键帧 [基于当前帧观测到的mappoint,把观测到这些mappoint的关键帧都添加进来]
    UpdateLocalPoints();    //更新局部地图mappoint [把上面得到的局部地图关键帧所有的mappoint添加到局部地图中]
}

void Tracking::UpdateLocalPoints()
{
    //清空局部地图关键点
    mvpLocalMapPoints.clear();

    //遍历局部地图关键帧
    for(vector<KeyFrame*>::const_iterator itKF=mvpLocalKeyFrames.begin(), itEndKF=mvpLocalKeyFrames.end(); itKF!=itEndKF; itKF++)
    {
        KeyFrame* pKF = *itKF;
        //取该关键帧的所有mappoint
        const vector<MapPoint*> vpMPs = pKF->GetMapPointMatches();

        //遍历该关键帧的每一个mappoint
        for(vector<MapPoint*>::const_iterator itMP=vpMPs.begin(), itEndMP=vpMPs.end(); itMP!=itEndMP; itMP++)
        {
            MapPoint* pMP = *itMP;
            if(!pMP)
                continue;
            // mnTrackReferenceForFrame防止重复添加局部MapPoint
            // 如果这个mappoint的mnTrackReferenceForFrame 为当前帧id,表示局部地图已经有这个mappoint了
            if(pMP->mnTrackReferenceForFrame==mCurrentFrame.mnId)
                continue;

            if(!pMP->isBad())
            {
                //局部地图添加当前这个mappoint
                mvpLocalMapPoints.push_back(pMP);
                //记录这个mappoint的mnTrackReferenceForFrame 为当前帧id
                pMP->mnTrackReferenceForFrame=mCurrentFrame.mnId;
            }
        }
    }
}


void Tracking::UpdateLocalKeyFrames()
{
    // Each map point vote for the keyframes in which it has been observed
    // 遍历当前帧的mappoint，将所有能观测到这些mappoint的keyframe，及其可以观测的这些mappoint数量存入keyframeCounter
    // keyframeCounter<某个关键帧,该关键帧与当前帧共视点数量>
    map<KeyFrame*,int> keyframeCounter;
    for(int i=0; i<mCurrentFrame.N; i++)
    {
        //遍历当前帧mappoint
        if(mCurrentFrame.mvpMapPoints[i])
        {
            MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];
            if(!pMP->isBad())
            { 
                // mObservations 记录此MapPoint被哪个关键帧观测到,对应该关键帧的哪个特征点idx
                // observations<哪个关键帧,关键帧特征点idx>
                const map<KeyFrame*,size_t> observations = pMP->GetObservations();
                for(map<KeyFrame*,size_t>::const_iterator it=observations.begin(), itend=observations.end(); it!=itend; it++)
                    keyframeCounter[it->first]++;
            }
            else
            {
                mCurrentFrame.mvpMapPoints[i]=NULL;
            }
        }
    }

    // keyframeCounter<某个关键帧,该关键帧与当前帧共视点数量>
    // 如果没有任何一个关键帧与当前帧有共视点,则返回
    if(keyframeCounter.empty())
        return;

    int max=0;
    KeyFrame* pKFmax= static_cast<KeyFrame*>(NULL);

    // 先清空局部地图关键帧
    // mvpLocalKeyFrames用来构造局部地图
    mvpLocalKeyFrames.clear();
    mvpLocalKeyFrames.reserve(3*keyframeCounter.size());

    // All keyframes that observe a map point are included in the local map. Also check which keyframe shares most points
    // 向mvpLocalKeyFrames添加能观测到当前帧MapPoints的关键帧
    for(map<KeyFrame*,int>::const_iterator it=keyframeCounter.begin(), itEnd=keyframeCounter.end(); it!=itEnd; it++)
    {
        //取一个与当前帧有共视点的关键帧pKF
        KeyFrame* pKF = it->first;

        if(pKF->isBad())
            continue;

        //更新max，pKFmax，以寻找能看到最多mappoint的keyframe
        //记录与当前帧有[最多]共视点的关键帧pKF
        if(it->second>max)
        {
            max=it->second;
            pKFmax=pKF;
        }

        //向mvpLocalKeyFrames添加能观测到当前帧MapPoints的关键帧
        mvpLocalKeyFrames.push_back(it->first);
        //mnTrackReferenceForFrame防止重复添加局部地图关键帧 ???
        //当前Frame对象id
        pKF->mnTrackReferenceForFrame = mCurrentFrame.mnId;
    }


    // Include also some not-already-included keyframes that are neighbors to already-included keyframes
    // 遍历mvpLocalKeyFrames，以向mvpLocalKeyFrames添加更多的关键帧。有三种途径：
    // 1.取出此关键帧itKF在Covisibilitygraph中共视程度最高的10个关键帧；
    // 2.取出此关键帧itKF在Spanning tree中的子节点；
    // 3.取出此关键帧itKF在Spanning tree中的父节点；
    for(vector<KeyFrame*>::const_iterator itKF=mvpLocalKeyFrames.begin(), itEndKF=mvpLocalKeyFrames.end(); itKF!=itEndKF; itKF++)
    {
        // Limit the number of keyframes
        if(mvpLocalKeyFrames.size()>80)
            break;

        KeyFrame* pKF = *itKF;

	//1.取出此关键帧itKF在essential graph中共视程度最高的10个关键帧
        const vector<KeyFrame*> vNeighs = pKF->GetBestCovisibilityKeyFrames(10);

        for(vector<KeyFrame*>::const_iterator itNeighKF=vNeighs.begin(), itEndNeighKF=vNeighs.end(); itNeighKF!=itEndNeighKF; itNeighKF++)
        {
            KeyFrame* pNeighKF = *itNeighKF;
            if(!pNeighKF->isBad())
            {
                if(pNeighKF->mnTrackReferenceForFrame!=mCurrentFrame.mnId)
                {
                    mvpLocalKeyFrames.push_back(pNeighKF);
                    pNeighKF->mnTrackReferenceForFrame=mCurrentFrame.mnId;
                    break;
                }
            }
        }

        //2.取出此关键帧itKF在Spanning tree中的子节点
        //Spanning tree的节点为关键帧，共视程度最高的那个关键帧设置为节点在Spanning Tree中的父节点
        const set<KeyFrame*> spChilds = pKF->GetChilds();
        for(set<KeyFrame*>::const_iterator sit=spChilds.begin(), send=spChilds.end(); sit!=send; sit++)
        {
            KeyFrame* pChildKF = *sit;
            if(!pChildKF->isBad())
            {
                if(pChildKF->mnTrackReferenceForFrame!=mCurrentFrame.mnId)
                {
                    mvpLocalKeyFrames.push_back(pChildKF);
                    pChildKF->mnTrackReferenceForFrame=mCurrentFrame.mnId;
                    break;
                }
            }
        }

        //3.取出此关键帧itKF在Spanning tree中的父节点
        KeyFrame* pParent = pKF->GetParent();
        if(pParent)
        {
            if(pParent->mnTrackReferenceForFrame!=mCurrentFrame.mnId)
            {
                mvpLocalKeyFrames.push_back(pParent);
                pParent->mnTrackReferenceForFrame=mCurrentFrame.mnId;
                break;
            }
        }

    }

    if(pKFmax)
    {
        //更新参考关键帧为: 与当前帧[共视的mappoint数量最多]的关键帧
        mpReferenceKF = pKFmax;
        mCurrentFrame.mpReferenceKF = mpReferenceKF;
    }
}

bool Tracking::Relocalization()
{
    // Compute Bag of Words Vector
    // 计算当前帧词袋向量
    mCurrentFrame.ComputeBoW();

    // Relocalization is performed when tracking is lost
    // Track Lost: Query KeyFrame Database for keyframe candidates for relocalisation
    // 找到与当前帧相似的候选关键帧
    vector<KeyFrame*> vpCandidateKFs = mpKeyFrameDB->DetectRelocalizationCandidates(&mCurrentFrame);

    //如果候选关键帧为空，则返回Relocalization失败
    if(vpCandidateKFs.empty())
        return false;

    //候选关键帧的数量
    const int nKFs = vpCandidateKFs.size();

    // We perform first an ORB matching with each candidate
    // If enough matches are found we setup a PnP solver
    // 首先使用ORB匹配器对每个候选关键帧进行匹配
    // 如果有足够的匹配,则开始求解Pnp
    ORBmatcher matcher(0.75,true);

    //每一个候选关键帧都分配一个vpPnPsolver?
    vector<PnPsolver*> vpPnPsolvers;
    vpPnPsolvers.resize(nKFs);

    //表示各个候选帧的mappoint与和当前帧特征点的匹配
    //现在你想把mCurrentFrame的特征点和mappoint进行匹配，有个便捷的方法就是，
    //让mCurrentFrame特征点和候选关键帧的特征点进行匹配,然后我们是知道候选关键帧特征点与mappoint的匹配的
    //这样就能够将mCurrentFrame特征点和mappoint匹配起来了，相当于通过和候选关键帧这个桥梁匹配上了mappoint
    //vvpMapPointMatches[i][j]就表示mCurrentFrame的第j个特征点如果是经由第i个候选关键帧匹配mappoint，是哪个mappoint
    //vvpMapPointMatches[第i个候选关键帧][当前帧第j个特征点]=第i个候选关键帧中与当前帧第j个特征点匹配的特征点对应的路标点mappoint
    vector<vector<MapPoint*> > vvpMapPointMatches;
    vvpMapPointMatches.resize(nKFs);

    //
    vector<bool> vbDiscarded;
    vbDiscarded.resize(nKFs);

    //匹配特征点数>15 的候选关键帧数量
    int nCandidates=0;

    //候选帧和当前帧进行特征匹配，剔除匹配数量少的候选关键帧
    //为未被剔除的关键帧就新建PnPsolver，准备在后面进行epnp
    for(int i=0; i<nKFs; i++)
    {
        //候选帧
        KeyFrame* pKF = vpCandidateKFs[i];
        if(pKF->isBad())
            vbDiscarded[i] = true;
        else
        {
            //mCurrentFrame与候选关键帧进行特征点匹配
            int nmatches = matcher.SearchByBoW(pKF,mCurrentFrame,vvpMapPointMatches[i]);
            if(nmatches<15) //匹配的特征点个数nmatches
            {
                vbDiscarded[i] = true;  //忽略这个候选关键帧
                continue;
            }
            else
            {
                //匹配特征点数>15,为这个候选关键帧创建一个PnpSolver
                //vvpMapPointMatches[第i个候选关键帧][当前帧第j个特征点]=第i个候选关键帧中与当前帧第j个特征点匹配的特征点对应的路标点mappoint
                PnPsolver* pSolver = new PnPsolver(mCurrentFrame,vvpMapPointMatches[i]);
                //pnp求解器参数设置
                pSolver->SetRansacParameters(0.99,10,300,4,0.5,5.991);
                vpPnPsolvers[i] = pSolver;
                nCandidates++;
            }
        }
    }

    // Alternatively perform some iterations of P4P RANSAC
    // Until we found a camera pose supported by enough inliers
    bool bMatch = false;
    ORBmatcher matcher2(0.9,true);

    //大概步骤是这样的，小循环for不断的遍历剩下的nCandidates个的候选帧，这些候选帧对应有各自的PnPsolvers
    //第i次for循环所对应的vpPnPsolvers[i]就会执行5次RANSAC循环求解出5个位姿。
    //通过计算5个位姿对应的匹配点的inliner数量来判断位姿的好坏。如果这5个位姿比记录中的最好位姿更好，更新最好位姿以及对应的匹配点哪些点是inliner
    //如果最好的那个位姿inliner超过阈值，或者vpPnPsolvers[i]RANSAC累计迭代次数超过阈值，都会把位姿拷贝给Tcw。否则Tcw为空
    //如果Tcw为空，那么就循环计算下一个vpPnPsolvers[i+1]
    //通过5次RANSAC求解位姿后，如果Tcw不为空，这继续判断它是否和当前帧匹配。
    while(nCandidates>0 && !bMatch)
    {
        //遍历候选帧
        for(int i=0; i<nKFs; i++)
        {
            //匹配的特征点个数太少,则忽略
            if(vbDiscarded[i])
                continue;

            // Perform 5 Ransac Iterations
            //此次RANSAC会计算出一个位姿，在这个位姿下，mCurrentFrame中的特征点哪些是有mappoint匹配的，也就是哪些是inliner
            //vbInliers大小是mCurrentFrame中的特征点数量大小
            vector<bool> vbInliers;
            //vbInliers大小
            int nInliers;
            bool bNoMore;

            PnPsolver* pSolver = vpPnPsolvers[i];
            //通过EPnP算法估计姿态Tcw，RANSAC迭代5次
            cv::Mat Tcw = pSolver->iterate(5,bNoMore,vbInliers,nInliers);

            // If Ransac reachs max. iterations discard keyframe
            //如果RANSAC循环达到了最大,还没有结果,则丢弃这个候选关键帧
            if(bNoMore)
            {
                vbDiscarded[i]=true;
                nCandidates--;
                //认真的虎注释：这里是不是应该加一个continue？
                //qpc : 加不加都可以, 反正后面都return false
            }

            // If a Camera Pose is computed, optimize
            // 如果相机姿态已经算出来了，优化它
            // 相机姿态算出来: RANSAC累计迭代次数没有达到mRansacMaxIts之前，找到了一个符合要求的位姿
            if(!Tcw.empty())
            {
                //将结果拷贝到当前帧位姿
                Tcw.copyTo(mCurrentFrame.mTcw);

                set<MapPoint*> sFound;

                //vbInliers大小是mCurrentFrame中的特征点数量大小
                //np为mCurrentFrame的特征点数量
                const int np = vbInliers.size();

                //根据vbInliers更新mCurrentFrame.mvpMapPoints，
                //也就是根据vbInliers更新mCurrentFrame的特征点与哪些mappoint匹配
                //并记下当前mCurrentFrame与哪些mappoint匹配到sFound，以便后面快速查询
                for(int j=0; j<np; j++)
                {
                    if(vbInliers[j])    //vbInliers 在RANSAC-EPnp求解中会被设置
                    {
                        //vvpMapPointMatches: BoW搜索得到的匹配
                        //vvpMapPointMatches[i][j]就表示mCurrentFrame的第j个特征点如果是经由第i个候选关键帧匹配mappoint，是哪个mappoint
                        //vvpMapPointMatches[第i个候选关键帧][当前帧第j个特征点]=第i个候选关键帧中与当前帧第j个特征点匹配的特征点对应的路标点mappoint
                        //mCurrentFrame.mvpMapPoints[j]=当前帧第j个特征点对应的路标点(从第i个候选关键帧中对应过来的)
                        mCurrentFrame.mvpMapPoints[j]=vvpMapPointMatches[i][j];

                        //将这个路标点插入
                        sFound.insert(vvpMapPointMatches[i][j]);
                    }
                    else
                        mCurrentFrame.mvpMapPoints[j]=NULL;
                }

                //BA优化位姿(前端优化)
                //返回好的边(关键点)数
                int nGood = Optimizer::PoseOptimization(&mCurrentFrame);

                //放弃这个候选关键帧
                if(nGood<10)
                    continue;

                //剔除BA优化时算出的mvbOutlier
                for(int io =0; io<mCurrentFrame.N; io++)
                    if(mCurrentFrame.mvbOutlier[io])
                        mCurrentFrame.mvpMapPoints[io]=static_cast<MapPoint*>(NULL);

                // If few inliers, search by projection in a coarse window and optimize again
                // 如果内点较少,从一个粗略的窗口投影搜索,再次优化
                if(nGood<50)
                {
                    // 如果内点较少,mCurrentFrame想要更多的mappoint匹配
                    // mCurrentFrame中特征点已经匹配好一些mappoint在sFound中
                    // 于是通过matcher2.SearchByProjection函数将vpCandidateKFs[i]的mappoint投影到CurrentFrame再就近搜索特征点进行匹配
                    // nadditional= 新增的成功匹配mappoint的数量
                    int nadditional =matcher2.SearchByProjection(mCurrentFrame,vpCandidateKFs[i],sFound,10,100);

                    //如果mCurrentFrame当前总共匹配到的mappoint个数超过50
                    if(nadditional+nGood>=50)
                    {
                        //优化位姿，返回(nadditional+nGood)有多少点是inliner
                        nGood = Optimizer::PoseOptimization(&mCurrentFrame);

                        // If many inliers but still not enough, search by projection again in a narrower window
                        // the camera has been already optimized with many points
                        // 如果nGood不够多，那缩小搜索框重复再匹配一次
                        if(nGood>30 && nGood<50)
                        {
                             //更新sFound，也就是目前mCurrentFrame与哪些mappoint匹配
                            sFound.clear();
                            for(int ip =0; ip<mCurrentFrame.N; ip++)
                                if(mCurrentFrame.mvpMapPoints[ip])
                                    sFound.insert(mCurrentFrame.mvpMapPoints[ip]);
                            //缩小搜索框重复再匹配一次,返回这个新得到的匹配数
                            nadditional =matcher2.SearchByProjection(mCurrentFrame,vpCandidateKFs[i],sFound,3,64);

                            // Final optimization
                            //如果目前mCurrentFrame匹配到的mappoint个数超过20
                            if(nGood+nadditional>=50)
                            {
                                //位姿优化，可能又会有些匹配消失
                                nGood = Optimizer::PoseOptimization(&mCurrentFrame);

                                //将刚才位姿优化得到的outliner匹配更新
                                for(int io =0; io<mCurrentFrame.N; io++)
                                    if(mCurrentFrame.mvbOutlier[io])
                                        mCurrentFrame.mvpMapPoints[io]=NULL;
                            }
                        }
                    }
                }


                // If the pose is supported by enough inliers stop ransacs and continue
                // 到这里,如果匹配点数>50,则认为找到好的候选关键帧,停止迭代
                if(nGood>=50)
                {
                    bMatch = true;
                    break;
                }
            }
        }
    }

    if(!bMatch)
    {
        return false;
    }
    else
    {
        //记录上一次Relocalization()使用的Frame ID，最近一次重定位帧的ID
        mnLastRelocFrameId = mCurrentFrame.mnId;
        return true;
    }

}

void Tracking::Reset()
{

    cout << "System Reseting" << endl;
    if(mpViewer)
    {
        mpViewer->RequestStop();
        while(!mpViewer->isStopped())
            usleep(3000);
    }

    // Reset Local Mapping
    cout << "Reseting Local Mapper...";
    mpLocalMapper->RequestReset();
    cout << " done" << endl;

    // Reset Loop Closing
    cout << "Reseting Loop Closing...";
    mpLoopClosing->RequestReset();
    cout << " done" << endl;

    // Clear BoW Database
    cout << "Reseting Database...";
    mpKeyFrameDB->clear();
    cout << " done" << endl;

    // Clear Map (this erase MapPoints and KeyFrames)
    mpMap->clear();

    KeyFrame::nNextId = 0;
    Frame::nNextId = 0;
    mState = NO_IMAGES_YET;

    if(mpInitializer)
    {
        delete mpInitializer;
        mpInitializer = static_cast<Initializer*>(NULL);
    }

    mlRelativeFramePoses.clear();
    mlpReferences.clear();
    mlFrameTimes.clear();
    mlbLost.clear();

    if(mpViewer)
        mpViewer->Release();
}

void Tracking::ChangeCalibration(const string &strSettingPath)
{
    cv::FileStorage fSettings(strSettingPath, cv::FileStorage::READ);
    float fx = fSettings["Camera.fx"];
    float fy = fSettings["Camera.fy"];
    float cx = fSettings["Camera.cx"];
    float cy = fSettings["Camera.cy"];

    cv::Mat K = cv::Mat::eye(3,3,CV_32F);
    K.at<float>(0,0) = fx;
    K.at<float>(1,1) = fy;
    K.at<float>(0,2) = cx;
    K.at<float>(1,2) = cy;
    K.copyTo(mK);

    cv::Mat DistCoef(4,1,CV_32F);
    DistCoef.at<float>(0) = fSettings["Camera.k1"];
    DistCoef.at<float>(1) = fSettings["Camera.k2"];
    DistCoef.at<float>(2) = fSettings["Camera.p1"];
    DistCoef.at<float>(3) = fSettings["Camera.p2"];
    const float k3 = fSettings["Camera.k3"];
    if(k3!=0)
    {
        DistCoef.resize(5);
        DistCoef.at<float>(4) = k3;
    }
    DistCoef.copyTo(mDistCoef);

    mbf = fSettings["Camera.bf"];

    Frame::mbInitialComputations = true;
}

void Tracking::InformOnlyTracking(const bool &flag)
{
    mbOnlyTracking = flag;
}



} //namespace ORB_SLAM
