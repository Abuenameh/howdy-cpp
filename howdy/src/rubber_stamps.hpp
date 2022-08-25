#ifndef RUBBER_STAMPS_H_
#define RUBBER_STAMPS_H_

#include <string>
#include <vector>
#include <variant>

#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>

#include <INIReader.h>

#include "process/process.hpp"

#include "video_capture.hpp"
#include "models.hpp"

using namespace TinyProcessLib;

struct OpenCV
{
	OpenCV(VideoCapture &video_capture, face_detection_model &face_detector, shape_predictor_model &pose_predictor, cv::Ptr<cv::CLAHE> clahe) : video_capture(video_capture), face_detector(face_detector), pose_predictor(pose_predictor), clahe(clahe)
	{
	}

	VideoCapture &video_capture;
	face_detection_model &face_detector;
	shape_predictor_model &pose_predictor;
	cv::Ptr<cv::CLAHE> clahe;
};

void execute(INIReader &config, std::shared_ptr<Process> gtk_proc, OpenCV &opencv);

#endif // RUBBER_STAMPS_H_
