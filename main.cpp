#include "braw_decoder.h"
#include "image_writer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Options {
    fs::path inputDirectory;
    fs::path outputDirectory;
    fs::path sdkPath;
    braw::OutputFormat format = braw::OutputFormat::Dng;
    uint64_t frameIndex = 0;
};

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " -i <input directory> -o <output directory> [-f dng|png]\n"
        << "\n"
        << "  -i, --inDir <directory>         directory containing .braw files\n"
        << "  -o, --outDir <directory>        directory to write converted stills to\n"
        << "  -f, --outputFormat <dng|png>    output format (default: dng)\n"
        << "      --frame <index>             frame to extract (default: 0)\n"
        << "      --sdkPath <directory>       Blackmagic RAW SDK libraries folder\n"
        << "  -h, --help                      show this message\n";
}

bool parseFormat(const std::string& value, braw::OutputFormat& format) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (lowered == "dng") {
        format = braw::OutputFormat::Dng;
        return true;
    }
    if (lowered == "png") {
        format = braw::OutputFormat::Png;
        return true;
    }
    return false;
}

bool parseArguments(int argc, char** argv, Options& options, bool& helpRequested, std::string& error) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto nextValue = [&](const char* optionName, std::string& value) {
            if (index + 1 >= argc) {
                error = std::string(optionName) + " requires an argument";
                return false;
            }
            value = argv[++index];
            return true;
        };

        std::string value;
        if (argument == "-h" || argument == "--help") {
            helpRequested = true;
            return true;
        }
        if (argument == "-i" || argument == "--inDir") {
            if (!nextValue("--inDir", value))
                return false;
            options.inputDirectory = fs::u8path(value);
        } else if (argument == "-o" || argument == "--outDir") {
            if (!nextValue("--outDir", value))
                return false;
            options.outputDirectory = fs::u8path(value);
        } else if (argument == "-f" || argument == "--outputFormat") {
            if (!nextValue("--outputFormat", value))
                return false;
            if (!parseFormat(value, options.format)) {
                error = "unknown output format '" + value + "' (expected dng or png)";
                return false;
            }
        } else if (argument == "--frame") {
            if (!nextValue("--frame", value))
                return false;
            try {
                options.frameIndex = std::stoull(value);
            } catch (const std::exception&) {
                error = "invalid frame index '" + value + "'";
                return false;
            }
        } else if (argument == "--sdkPath") {
            if (!nextValue("--sdkPath", value))
                return false;
            options.sdkPath = fs::u8path(value);
        } else {
            error = "unknown option '" + argument + "'";
            return false;
        }
    }
    return true;
}

bool hasBrawExtension(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension == ".braw";
}

std::vector<fs::path> collectClips(const fs::path& directory, std::string& error) {
    std::vector<fs::path> clips;
    std::error_code code;
    for (fs::directory_iterator entry(directory, code), end; !code && entry != end; entry.increment(code)) {
        std::error_code statusCode;
        if (!entry->is_regular_file(statusCode) || statusCode)
            continue;
        if (hasBrawExtension(entry->path()))
            clips.push_back(entry->path());
    }
    if (code) {
        error = "failed to read '" + directory.string() + "': " + code.message();
        return {};
    }
    std::sort(clips.begin(), clips.end());
    return clips;
}

fs::path outputPathFor(const Options& options, const fs::path& clip) {
    return options.outputDirectory /
           (clip.stem().string() +
            (options.format == braw::OutputFormat::Dng ? ".dng" : ".png"));
}

bool convertClip(const Options& options, const fs::path& clip) {
    braw::RenderOptions renderOptions;
    renderOptions.format = options.format;
    renderOptions.sdkLibraryPath = options.sdkPath;
    renderOptions.frameIndex = options.frameIndex;

    braw::RenderResult rendered;
    std::string error;

    const auto start = std::chrono::steady_clock::now();
    if (!braw::renderFrame(clip, renderOptions, rendered, error)) {
        std::cerr << "  error: " << error << std::endl;
        return false;
    }

    const fs::path outputPath = outputPathFor(options, clip);
    bool written = false;
    if (options.format == braw::OutputFormat::Png) {
        written = image_writer::writePng(outputPath, rendered.width, rendered.height,
                                         rendered.rgba, error);
    } else {
        image_writer::DngImage image;
        image.width = rendered.width;
        image.height = rendered.height;
        image.rgb = rendered.rgb.data();
        image.whiteLevel = rendered.whiteLevel;
        image.make = "Blackmagic Design";
        image.model = rendered.cameraType;
        image.uniqueCameraModel =
            rendered.cameraType.empty() ? "Blackmagic Design" : "Blackmagic Design " + rendered.cameraType;
        std::copy(std::begin(rendered.xyzToCamera), std::end(rendered.xyzToCamera),
                  std::begin(image.colorMatrix1));
        written = image_writer::writeDng(outputPath, image, error);
    }

    if (!written) {
        std::cerr << "  error: " << error << std::endl;
        return false;
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "  wrote " << outputPath.string() << " (" << rendered.width << "x"
              << rendered.height;
    if (options.format == braw::OutputFormat::Dng)
        std::cout << ", gamut " << rendered.colorGamut;
    std::cout << ", " << std::fixed << std::setprecision(2) << seconds << "s)" << std::endl;

    if (options.format == braw::OutputFormat::Dng && rendered.maxLinearValue > 1.0f) {
        std::cout << std::setprecision(2) << "  note: clipped highlights above 1.0 (max linear "
                  << rendered.maxLinearValue << "); use -f png for a tone mapped preview"
                  << std::endl;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    bool helpRequested = false;
    std::string error;

    if (!parseArguments(argc, argv, options, helpRequested, error)) {
        std::cerr << "error: " << error << "\n\n";
        printUsage(argv[0]);
        return 1;
    }
    if (helpRequested) {
        printUsage(argv[0]);
        return 0;
    }

    if (options.inputDirectory.empty() || options.outputDirectory.empty()) {
        std::cerr << "error: both --inDir and --outDir are required\n\n";
        printUsage(argv[0]);
        return 1;
    }

    std::error_code code;
    if (!fs::is_directory(options.inputDirectory, code)) {
        std::cerr << "error: input directory '" << options.inputDirectory.string()
                  << "' does not exist" << std::endl;
        return 1;
    }
    if (!fs::is_directory(options.outputDirectory, code)) {
        fs::create_directories(options.outputDirectory, code);
        if (code) {
            std::cerr << "error: could not create output directory '"
                      << options.outputDirectory.string() << "': " << code.message() << std::endl;
            return 1;
        }
    }

    if (options.sdkPath.empty()) {
        const char* fromEnvironment = std::getenv("BRAW_SDK_PATH");
        options.sdkPath = fromEnvironment != nullptr && *fromEnvironment != '\0'
                              ? fs::u8path(fromEnvironment)
                              : braw::defaultSdkLibraryPath();
    }
    if (options.sdkPath.empty() || !fs::is_directory(options.sdkPath, code)) {
        std::cerr << "error: could not find the Blackmagic RAW SDK libraries. Install the "
                     "Blackmagic RAW SDK or pass --sdkPath <directory>." << std::endl;
        return 1;
    }

    const std::vector<fs::path> clips = collectClips(options.inputDirectory, error);
    if (!error.empty()) {
        std::cerr << "error: " << error << std::endl;
        return 1;
    }
    if (clips.empty()) {
        std::cout << "No .braw files found in '" << options.inputDirectory.string() << "'."
                  << std::endl;
        return 0;
    }

    if (!braw::initialize(error)) {
        std::cerr << "error: " << error << std::endl;
        return 1;
    }

    std::cout << "Blackmagic RAW SDK: " << options.sdkPath.string() << "\n"
              << "Input: " << options.inputDirectory.string() << "\n"
              << "Output: " << options.outputDirectory.string() << "\n"
              << "Format: " << (options.format == braw::OutputFormat::Dng ? "dng" : "png") << "\n"
              << "----------" << std::endl;

    size_t failures = 0;
    for (const fs::path& clip : clips) {
        std::cout << clip.filename().string() << std::endl;
        if (!convertClip(options, clip))
            ++failures;
    }

    braw::shutdown();

    std::cout << "----------\n"
              << (clips.size() - failures) << " of " << clips.size() << " file(s) converted."
              << std::endl;
    return failures == 0 ? 0 : 1;
}
