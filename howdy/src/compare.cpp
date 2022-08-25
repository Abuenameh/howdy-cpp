#include <sys/syslog.h>
#include <syslog.h>
#include <unistd.h>
#include <limits.h>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <map>
#include <iomanip>
#include <ctime>

#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core/utils/logger.hpp>

#include <dlib/opencv.h>
#include <dlib/dnn.h>
#include <dlib/image_processing/frontal_face_detector.h>

#include <INIReader.h>

#include "video_capture.hpp"
#include "models.hpp"
#include "snapshot.hpp"
#include "rubber_stamps.hpp"
#include "utils.hpp"

#include "utils/json.hpp"
#include "process/process.hpp"

#define FMT_HEADER_ONLY
#include "fmt/core.h"
#include "fmt/chrono.h"

using json = nlohmann::json;
using namespace dlib;
using namespace TinyProcessLib;

namespace fs = std::filesystem;

typedef std::chrono::time_point<std::chrono::system_clock> time_point;

std::shared_ptr<Process> gtk_proc;
std::function<void(const char *bytes, size_t n)> gtk_pipe;

/*Exit while closeing howdy-gtk properly*/
void exit_gtk()
{
    // Exit the auth ui process if there is one
    if (gtk_proc)
        gtk_proc->kill(true);
}

/*Send message to the auth ui*/
void send_to_ui(std::string type, std::string message)
{
    // Only execute of the proccess started
    if (gtk_proc)
    {
        // Format message so the ui can parse it
        message = type + "=" + message + " \n";

        // Try to send the message to the auth ui, but it's okay if that fails
        gtk_proc->write(message);
    }
}

int main(int argc, char *argv[])
{
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);

    std::map<std::string, time_point> start_times;
    std::map<std::string, std::chrono::duration<double>> timings;

    start_times["st"] = now();

    openlog("howdy-auth", 0, LOG_AUTHPRIV);

    // Make sure we were given an username to test against
    if (argc < 2)
    {
        exit(12);
    }

    // The username of the user being authenticated
    char *user = argv[1];
    // The model file contents
    json models;
    // Encoded face models
    std::vector<matrix<double, 0, 1>> encodings;
    // Amount of ignored 100% black frames
    int black_tries = 0;
    // Amount of ingnored dark frames
    int dark_tries = 0;
    // Total amount of frames captured
    int frames = 0;
    // Captured frames for snapshot capture
    std::vector<cv::Mat> snapframes;
    // Tracks the lowest certainty value in the loop
    double lowest_certainty = 10;

    // Try to load the face model from the models folder
    if (!fs::exists(fs::status(PATH + "/models/" + user + ".dat")))
    {
        syslog(LOG_ERR, "Model file not found for user %s", user);
        exit(10);
    }
    std::ifstream f(PATH + "/models/" + user + ".dat");
    models = json::parse(f);
    for (auto &model : models)
    {
        for (auto &row : model["data"])
        {
            encodings.push_back(mat(std::vector<double>(row)));
        }
    }

    // Check if the file contains a model
    if (models.size() < 1)
    {
        exit(10);
    }

    // Read config from disk
    INIReader config(PATH + "/config.ini");

    // Error out if we could not read the config file
    if (config.ParseError() != 0)
    {
        syslog(LOG_ERR, "Failed to parse the configuration file: %d", config.ParseError());
        exit(10);
    }

    // Get all config values needed
    bool use_cnn = config.GetBoolean("core", "use_cnn", false);
    int timeout = config.GetInteger("video", "timeout", 5);
    double dark_threshold = config.GetReal("video", "dark_threshold", 50.0);
    double video_certainty = config.GetReal("video", "certainty", 3.5) / 10;
    bool end_report = config.GetBoolean("debug", "end_report", false);
    bool capture_failed = config.GetBoolean("snapshots", "capture_failed", false);
    bool capture_successful = config.GetBoolean("snapshots", "capture_successful", false);
    bool gtk_stdout = config.GetBoolean("debug", "gtk_stdout", false);
    int rotate = config.GetInteger("video", "rotate", 0);

    // Send the gtk outupt to the terminal if enabled in the config
    if (gtk_stdout)
        gtk_pipe = [](const char *bytes, size_t n)
        { std::cout << std::string{bytes, n}; };
    else
        gtk_pipe = nullptr;

    // Start the auth ui, register it to be always be closed on exit
    gtk_proc = std::make_shared<Process>("/lib64/security/howdy-gtk/howdy-gtk --start-auth-ui", "", gtk_pipe, gtk_pipe, true);
    std::atexit(exit_gtk);

    // Write to the stdin to redraw ui
    send_to_ui("M", "Starting up...");

    // Save the time needed to start the script
    timings["in"] = now() - start_times["st"];

    // Import face recognition, takes some time
    start_times["ll"] = now();

    if (!fs::is_regular_file(fs::status(PATH + "/dlib-data/shape_predictor_5_face_landmarks.dat")))
    {
        syslog(LOG_ERR, "Data files have not been downloaded");
        exit(1);
    }

    face_detection_model *face_detector_p;
    if (use_cnn)
    {
        face_detector_p = new cnn_face_detection_model_v1(PATH + "/dlib-data/mmod_human_face_detector.dat");
    }
    else
    {
        face_detector_p = new frontal_face_detector_model();
    }
    face_detection_model &face_detector = *face_detector_p;

    // Start the others regardless
    shape_predictor_model pose_predictor = shape_predictor_model(PATH + "/dlib-data/shape_predictor_5_face_landmarks.dat");
    face_recognition_model_v1 face_encoder = face_recognition_model_v1(PATH + "/dlib-data/dlib_face_recognition_resnet_model_v1.dat");

    // Note the time it took to initialize detectors
    timings["ll"] = now() - start_times["ll"];

    // Start video capture on the IR camera
    start_times["ic"] = now();

    VideoCapture video_capture(config);

    // Read exposure from config to use in the main loop
    int exposure = config.GetInteger("video", "exposure", -1);

    // Note the time it took to open the camera
    timings["ic"] = now() - start_times["ic"];

    // Fetch the max frame height
    double max_height = config.GetReal("video", "max_height", 0.0);

    // Get the height of the image (which would be the width if screen is portrait oriented)
    double height;
    height = video_capture.get(cv::CAP_PROP_FRAME_HEIGHT);
    if (rotate == 2)
    {
        height = video_capture.get(cv::CAP_PROP_FRAME_WIDTH);
    }
    if (height == 0)
    {
        height = 1;
    }
    // Calculate the amount the image has to shrink
    double scaling_factor = max_height / height;
    if (scaling_factor == 0)
    {
        scaling_factor = 1;
    }

    // Initiate histogram equalization
    auto clahe = cv::createCLAHE(2.0, cv::Size(8, 8));

    // Let the ui know that we're ready
    send_to_ui("M", "Identifying you...");

    // Start the read loop
    frames = 0;
    int valid_frames = 0;
    start_times["fr"] = now();
    double dark_running_total = 0;

    /* Generate snapshot after detection */
    auto make_snapshot = [&](std::string type)
    {
        char hostname[HOST_NAME_MAX];
        gethostname(hostname, HOST_NAME_MAX);

        std::vector<std::string> text_lines{
            type + " LOGIN",
            "Date: " + fmt::format("{:%Y/%m/%d %H:%M:%S} UTC", now()),
            "Scan time: " + fmt::format("{:.2f}", std::chrono::duration_cast<std::chrono::milliseconds>(now() - start_times["fr"]).count() / 1000.0) + "s",
            "Frames: " + std::to_string(frames) + " (" + fmt::format("{:.2f}", double(frames) / std::chrono::duration_cast<std::chrono::seconds>(now() - start_times["fr"]).count()) + "FPS)",
            "Hostname: " + std::string(hostname),
            "Best certainty value: " + fmt::format("{:.1f}", lowest_certainty * 10)};
        generate(snapframes, text_lines);
    };

    while (true)
    {
        // Increment the frame count every loop
        frames += 1;

        // Form a string to let the user know we're real busy
        std::string ui_subtext = "Scanned " + std::to_string(valid_frames - dark_tries) + " frames";
        if (dark_tries > 1)
        {
            ui_subtext += " (skipped " + std::to_string(dark_tries) + " dark frames)";
        }
        // Show it in the ui as subtext
        send_to_ui("S", ui_subtext);

        // Stop if we've exceded the time limit
        if (std::chrono::duration<double>(now() - start_times["fr"]).count() > timeout)
        {
            // Create a timeout snapshot if enabled
            if (capture_failed)
            {
                make_snapshot("FAILED");
            }

            if (dark_tries == valid_frames)
            {
                syslog(LOG_ERR, "All frames were too dark, please check dark_threshold in config");
                syslog(LOG_ERR, "Average darkness: %f, Threshold: %f", dark_running_total / std::max(1, valid_frames), dark_threshold);
                exit(13);
            }
            else
            {
                exit(11);
            }
        }

        // Grab a single frame of video
        cv::Mat tempframe;
        cv::Mat frame, gsframe;
        video_capture.read_frame(frame, tempframe);
        clahe->apply(tempframe, gsframe);

        // If snapshots have been turned on
        if (capture_failed || capture_successful)
        {
            // Start capturing frames for the snapshot
            if (snapframes.size() < 3)
                snapframes.push_back(frame);
        }

        // Create a histogram of the image with 8 values
        cv::Mat hist;
        cv::calcHist(std::vector<cv::Mat>{gsframe}, std::vector<int>{0}, cv::Mat(), hist, std::vector<int>{8}, std::vector<float>{0, 256});
        // All values combined for percentage calculation
        double hist_total = cv::sum(hist)[0];

        // Calculate frame darkness
        double darkness = (hist.at<float>(0) / hist_total * 100);

        // If the image is fully black due to a bad camera read,
        // skip to the next frame
        if ((hist_total == 0) or (darkness == 100))
        {
            black_tries += 1;
            continue;
        }

        dark_running_total += darkness;
        valid_frames += 1;
        // If the image exceeds darkness threshold due to subject distance,
        // skip to the next frame
        if (darkness > dark_threshold)
        {
            dark_tries += 1;
            continue;
        }

        // If the height is too high
        if (scaling_factor != 1)
        {
            // Apply that factor to the frame
            cv::resize(frame, tempframe, cv::Size(), scaling_factor, scaling_factor, cv::INTER_AREA);
            frame = tempframe;
            cv::resize(gsframe, tempframe, cv::Size(), scaling_factor, scaling_factor, cv::INTER_AREA);
            gsframe = tempframe;
        }
        // If camera is configured to rotate = 1, check portrait in addition to landscape
        if (rotate == 1)
        {
            if (frames % 3 == 1)
            {
                cv::rotate(frame, tempframe, cv::ROTATE_90_COUNTERCLOCKWISE);
                frame = tempframe;
                cv::rotate(gsframe, tempframe, cv::ROTATE_90_COUNTERCLOCKWISE);
                gsframe = tempframe;
            }
            if (frames % 3 == 2)
            {
                cv::rotate(frame, tempframe, cv::ROTATE_90_CLOCKWISE);
                frame = tempframe;
                cv::rotate(gsframe, tempframe, cv::ROTATE_90_CLOCKWISE);
                gsframe = tempframe;
            }
        }
        // If camera is configured to rotate = 2, check portrait orientation
        else if (rotate == 2)
        {
            if (frames % 2 == 0)
            {
                cv::rotate(frame, tempframe, cv::ROTATE_90_COUNTERCLOCKWISE);
                frame = tempframe;
                cv::rotate(gsframe, tempframe, cv::ROTATE_90_COUNTERCLOCKWISE);
                gsframe = tempframe;
            }
            else
            {
                cv::rotate(frame, tempframe, cv::ROTATE_90_CLOCKWISE);
                frame = tempframe;
                cv::rotate(gsframe, tempframe, cv::ROTATE_90_CLOCKWISE);
                gsframe = tempframe;
            }
        }

        // Get all faces from that frame as encodings
        // Upsamples 1 time
        std::vector<rectangle> face_locations = face_detector(gsframe, 1);

        // Loop through each face
        for (auto &&fl : face_locations)
        {
            // Fetch the faces in the image
            auto face_landmark = pose_predictor(frame, fl);
            auto face_encoding = face_encoder.compute_face_descriptor(frame, face_landmark, 1);

            // Match this found face against a known face
            std::vector<double> matches;
            std::transform(encodings.begin(), encodings.end(), std::back_inserter(matches), [face_encoding](auto &encoding)
                           { return sqrt(sum(squared(encoding - face_encoding))); });

            // Get best match
            int match_index = static_cast<int>(std::distance(matches.begin(), min_element(matches.begin(), matches.end())));
            double match = matches[match_index];

            // Update certainty if we have a new low
            if (lowest_certainty > match)
            {
                lowest_certainty = match;
            }

            // Check if a match that's confident enough
            if (0 < match && match < video_certainty)
            {
                timings["tt"] = now() - start_times["st"];
                timings["fl"] = now() - start_times["fr"];

                // If set to true in the config, print debug text
                if (end_report)
                {
                    /*
                    Helper function to print a timing from the list
                    */
                    auto syslog_timing = [&timings](std::string label, std::string k)
                    {
                        syslog(LOG_INFO, "  %s: %dms", label.c_str(), int(round(timings[k].count() * 1000)));
                    };

                    // Print a nice timing report
                    syslog(LOG_INFO, "Time spent");
                    syslog_timing("Starting up", "in");
                    syslog(LOG_INFO, "  Open cam + load libs: %dms", int(round(std::max(timings["ll"].count(), timings["ic"].count()) * 1000)));
                    syslog_timing("  Opening the camera", "ic");
                    syslog_timing("  Importing recognition libs", "ll");
                    syslog_timing("Searching for known face", "fl");
                    syslog_timing("Total time", "tt");

                    syslog(LOG_INFO, "\nResolution");
                    double width = video_capture.fw;
                    if (width == 0)
                    {
                        width = 1;
                    }
                    syslog(LOG_INFO, "  Native: %dx%d", int(height), int(width));
                    // Save the new size for diagnostics
                    int scale_height = frame.rows;
                    int scale_width = frame.cols;
                    syslog(LOG_INFO, "  Used: %dx%d", scale_height, scale_width);

                    // Show the total number of frames and calculate the FPS by deviding it by the total scan time
                    syslog(LOG_INFO, "\nFrames searched: %d (%.2f fps)", frames, frames / timings["fl"].count());
                    syslog(LOG_INFO, "Black frames ignored: %d ", black_tries);
                    syslog(LOG_INFO, "Dark frames ignored: %d ", dark_tries);
                    syslog(LOG_INFO, "Certainty of winning frame: %.3f", match * 10);

                    syslog(LOG_INFO, "Winning model: %d (\"%s\")", match_index, std::string(models[match_index]["label"]).c_str());
                }
                // Make snapshot if enabled
                if (capture_successful)
                {
                    make_snapshot("SUCCESSFUL");
                }

                // Run rubberstamps if enabled
                if (config.GetBoolean("rubberstamps", "enabled", false))
                {
                    OpenCV opencv(video_capture, face_detector, pose_predictor, clahe);
                    execute(config, gtk_proc, opencv);

                    send_to_ui("S", "");
                }

                // End peacefully
                exit(0);
            }
        }
        if (exposure != -1)
        {
            // For a strange reason on some cameras (e.g. Lenoxo X1E) setting manual exposure works only after a couple frames
            // are captured and even after a delay it does not always work. Setting exposure at every frame is reliable though.
            video_capture.set(cv::CAP_PROP_AUTO_EXPOSURE, 1.0); // 1 = Manual
            video_capture.set(cv::CAP_PROP_EXPOSURE, double(exposure));
        }
    }
}