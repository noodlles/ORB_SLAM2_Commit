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

#ifndef KEYFRAME_H
#define KEYFRAME_H

#include "MapPoint.h"
#include "Thirdparty/DBoW2/DBoW2/BowVector.h"
#include "Thirdparty/DBoW2/DBoW2/FeatureVector.h"
#include "ORBVocabulary.h"
#include "ORBextractor.h"
#include "Frame.h"
#include "KeyFrameDatabase.h"

#include <mutex>


namespace ORB_SLAM2
{

class Map;
class MapPoint;
class Frame;
class KeyFrameDatabase;

class KeyFrame
{
public:
    KeyFrame(Frame &F, Map* pMap, KeyFrameDatabase* pKFDB);

    // Pose functions
    void SetPose(const cv::Mat &Tcw);
    cv::Mat GetPose();
    cv::Mat GetPoseInverse();
    cv::Mat GetCameraCenter();
    cv::Mat GetStereoCenter();
    cv::Mat GetRotation();
    cv::Mat GetTranslation();

    // Bag of Words Representation
    //计算此关键帧的mBowVec，mFeatVec
    void ComputeBoW();

    // Covisibility graph functions
    /**在共视图Covisibility graph中添加边，并调用UpdateBestCovisibles()更新essential graph
     * @param pKF 具有共视关系的其他关键帧
     * @param weight 和pKF共视的mappoint数量
     */
    void AddConnection(KeyFrame* pKF, const int &weight);
    void EraseConnection(KeyFrame* pKF);
    
    
    //更新共视图Covisibility graph,essential graph和spanningtree，以及共视关系
    void UpdateConnections();
    //更新 mvpOrderedConnectedKeyFrames,mvOrderedWeights,也就是共视图covisibility
    void UpdateBestCovisibles();
    //返回此关键帧有共视关系的节点
    std::set<KeyFrame *> GetConnectedKeyFrames();
    //返回Covisibility graph中与此节点连接的节点（即关键帧）
    std::vector<KeyFrame* > GetVectorCovisibleKeyFrames();
    //返回Covisibility graph中与此节点连接的权值前N的节点
    std::vector<KeyFrame*> GetBestCovisibilityKeyFrames(const int &N);
    std::vector<KeyFrame*> GetCovisiblesByWeight(const int &w);
    int GetWeight(KeyFrame* pKF);

    // Spanning tree functions
    //Spanning tree的节点为关键帧，共视程度最高的那个关键帧设置为节点在Spanning Tree中的父节点
    void AddChild(KeyFrame* pKF);
    void EraseChild(KeyFrame* pKF);
    void ChangeParent(KeyFrame* pKF);
    std::set<KeyFrame*> GetChilds();
    KeyFrame* GetParent();
    bool hasChild(KeyFrame* pKF);

    // Loop Edges
    //添加一个闭环检测帧
    void AddLoopEdge(KeyFrame* pKF);
    std::set<KeyFrame*> GetLoopEdges();

    // MapPoint observation functions
    /**向关键帧添加mappoint，让keyframe知道自己可以看到哪些mappoint
     * @param pMP 添加的mappoint
     * @param idx mappoint在此帧对应的特征点的序号
     */
    void AddMapPoint(MapPoint* pMP, const size_t &idx);
    void EraseMapPointMatch(const size_t &idx);
    void EraseMapPointMatch(MapPoint* pMP);
    void ReplaceMapPointMatch(const size_t &idx, MapPoint* pMP);
    std::set<MapPoint*> GetMapPoints();
    //外部接口,大小是mvKeys大小，表示mappoint和此帧特征点的联系。如果没有联系则为NULL
    std::vector<MapPoint*> GetMapPointMatches();
    //返回这个关键帧可以观测到的mappoint里面,这些mappoint被观测到minObs次以上的数量
    int TrackedMapPoints(const int &minObs);
    MapPoint* GetMapPoint(const size_t &idx);

    // KeyPoint functions
    /**参考Frame的GetFeaturesInArea
      * 找到在 以x, y为中心,边长为2r的方形内的特征点
      * @param x        图像坐标u
      * @param y        图像坐标v
      * @param r        边长
      * @return         满足条件的特征点的序号
      */
    std::vector<size_t> GetFeaturesInArea(const float &x, const float  &y, const float  &r) const;
    cv::Mat UnprojectStereo(int i);

    // Image
    bool IsInImage(const float &x, const float &y) const;

    // Enable/Disable bad flag changes
    
    // Avoid that a keyframe can be erased while it is being process by loop closing
    void SetNotErase();
    void SetErase();

    // Set/check bad flag
    void SetBadFlag();
    bool isBad();

    // Compute Scene Depth (q=2 median). Used in monocular.
    //返回mappoint集合在此帧的深度的中位数
    float ComputeSceneMedianDepth(const int q);

    static bool weightComp( int a, int b){
        return a>b;
    }

    static bool lId(KeyFrame* pKF1, KeyFrame* pKF2){
        return pKF1->mnId<pKF2->mnId;
    }


    // The following variables are accesed from only 1 thread or never change (no mutex needed).
public:

    //下一个keyframe的序号
    static long unsigned int nNextId;
    //当前keyframe的序号
    long unsigned int mnId;
    //生成这个keyframe的Frame id
    const long unsigned int mnFrameId;

    const double mTimeStamp;

    // Grid (to speed up feature matching)
    const int mnGridCols;
    const int mnGridRows;
    const float mfGridElementWidthInv;
    const float mfGridElementHeightInv;

    // Variables used by the tracking
    //防止重复添加局部地图关键帧
    long unsigned int mnTrackReferenceForFrame;
    //在LocalMapping::SearchInNeighbors()中，标记此keyframe将要融合（fuse）的keyframe ID
    long unsigned int mnFuseTargetForKF;

    // Variables used by the local mapping
    //local mapping的local BA在使用此关键帧的标记
    long unsigned int mnBALocalForKF;
    long unsigned int mnBAFixedForKF;

    // Variables used by the keyframe database
    //此为frame ID，标记是哪个keyframe查询过和此keyframe有相同的单词，且在essentialgraph中连接
    long unsigned int mnLoopQuery;
    //mnLoopQuery指向的Frame和此keyframe有多少共同的单词
    int mnLoopWords;
    //此为通过dbow计算的mnLoopQuery指向的Frame与此keyframe之间相似度的得分
    float mLoopScore;
    
    //此为frame ID，标记是哪个frame查询过和此keyframe有相同的单词
    long unsigned int mnRelocQuery;
    //mnRelocQuery指向的Frame和此keyframe有多少共同的单词
    int mnRelocWords;
    //当mnRelocWords大于阈值值时，就会被计算此值。
    //此为通过dbow计算的mnRelocQuery指向的Frame与此keyframe之间相似度的得分
    float mRelocScore;

    // Variables used by loop closing
    //global BA的结果
    cv::Mat mTcwGBA;
    cv::Mat mTcwBefGBA;
    long unsigned int mnBAGlobalForKF;

    // Calibration parameters
    const float fx, fy, cx, cy, invfx, invfy, mbf, mb, mThDepth;
    //mbf : 基线*fx

    // Number of KeyPoints
    const int N;

    // KeyPoints, stereo coordinate and descriptors (all associated by an index)
    const std::vector<cv::KeyPoint> mvKeys;
    //
    const std::vector<cv::KeyPoint> mvKeysUn;
    const std::vector<float> mvuRight; // negative value for monocular points
    const std::vector<float> mvDepth; // negative value for monocular points
    const cv::Mat mDescriptors;

    //BoW
    //mBowVec本质是一个map<WordId, WordValue>
    //对于某幅图像A，它的特征点可以对应多个单词，组成它的bow
    DBoW2::BowVector mBowVec;
    //mFeatVec是一个std::map<NodeId, std::vector<unsigned int> >
    //将此帧的特征点分配到mpORBVocabulary树各个结点，从而得到mFeatVec
    //mFeatVec->first代表结点ID
    //mFeatVec->second代表在mFeatVec->first结点的特征点序号的vector集合
    DBoW2::FeatureVector mFeatVec;

    // Pose relative to parent (this is computed when bad flag is activated)
    cv::Mat mTcp;

    // Scale
    const int mnScaleLevels;
    const float mfScaleFactor;
    const float mfLogScaleFactor;
    const std::vector<float> mvScaleFactors;
    const std::vector<float> mvLevelSigma2;
    const std::vector<float> mvInvLevelSigma2;

    // Image bounds and calibration
    const int mnMinX;
    const int mnMinY;
    const int mnMaxX;
    const int mnMaxY;
    const cv::Mat mK;


    // The following variables need to be accessed trough a mutex to be thread safe.
protected:

    // SE3 Pose and camera center
    cv::Mat Tcw;
    cv::Mat Twc;
    //关键帧光心在世界坐标系中的坐标
    cv::Mat Ow;

    cv::Mat Cw; // Stereo middel point. Only for visualization

    // MapPoints associated to keypoints
    //此keyframe可以看到哪些mappoint
    //大小是mvKeys大小，表示mappoint和此帧特征点的联系。如果没有联系则为NULL
    std::vector<MapPoint*> mvpMapPoints;

    // BoW
    KeyFrameDatabase* mpKeyFrameDB;
    ORBVocabulary* mpORBvocabulary;

    // Grid over the image to speed up feature matching
    //储存这各个窗格的特征点在mvKeysUn中的序号
    std::vector< std::vector <std::vector<size_t> > > mGrid;

    //与此关键帧其他关键帧的共视关系及其mappoint共视数量
    std::map<KeyFrame*,int> mConnectedKeyFrameWeights;
    
    //mvpOrderedConnectedKeyFrames和mvOrderedWeights共同组成了论文里的covisibility graph
    //与此关键帧具有连接关系的关键帧，其顺序按照共视的mappoint数量递减排序
    std::vector<KeyFrame*> mvpOrderedConnectedKeyFrames;
    //mvpOrderedConnectedKeyFrames中共视的mappoint数量，也就是共视图covisibility graph权重，递减排列
    std::vector<int> mvOrderedWeights;

    // Spanning Tree and Loop Edges
    //此keyframe
    bool mbFirstConnection;
    
    //此keyframe在Spanning Tree中的父节点
    KeyFrame* mpParent;
    //此keyframe在Spanning Tree中的子节点集合
    std::set<KeyFrame*> mspChildrens;
    std::set<KeyFrame*> mspLoopEdges;

    // Bad flags
    // Avoid that a keyframe can be erased while it is being process by loop closing
    bool mbNotErase;
    bool mbToBeErased;
    bool mbBad;    

    float mHalfBaseline; // Only for visualization

    Map* mpMap;

    std::mutex mMutexPose;
    std::mutex mMutexConnections;
    std::mutex mMutexFeatures;
};

} //namespace ORB_SLAM

#endif // KEYFRAME_H
