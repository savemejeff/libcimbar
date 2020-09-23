/* This code is subject to the terms of the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "cimb_translator/Config.h"
#include "compression/zstd_decompressor.h"
#include "encoder/Decoder.h"
#include "encoder/FileEncoder.h"
#include "extractor/Extractor.h"
#include "extractor/SimpleCameraCalibration.h"
#include "extractor/Undistort.h"
#include "fountain/FountainInit.h"
#include "fountain/fountain_decoder_sink.h"
#include "serialize/str.h"

#include "cxxopts/cxxopts.hpp"

#include <functional>
#include <iostream>
#include <string>
#include <vector>
using std::string;
using std::vector;

int decode(const vector<string>& infiles, const std::function<int(cv::UMat, bool)>& decode, bool no_deskew, bool undistort, int preprocess)
{
	int err = 0;
	for (const string& inf : infiles)
	{
		bool shouldPreprocess = (preprocess == 1);
		cv::UMat img = cv::imread(inf).getUMat(cv::ACCESS_RW);
		if (!no_deskew)
		{
			// attempt undistort
			// we don't fail outright, but we'll probably fail the decode :(
			if (undistort)
			{
				Undistort<SimpleCameraCalibration> und;
				if (!und.undistort(img, img))
					err |= 1;
			}

			Extractor ext;
			int res = ext.extract(img, img);
			if (!res)
			{
				err |= 2;
				continue;
			}
			else if (preprocess != 0 and res == Extractor::NEEDS_SHARPEN)
				shouldPreprocess = true;
		}

		int bytes = decode(img, shouldPreprocess);
		if (!bytes)
			err |= 4;
	}
	return err;
}

template <typename SINK>
std::function<int(cv::UMat,bool)> fountain_decode_fun(SINK& sink, Decoder& d)
{
	return [&sink, &d] (cv::UMat m, bool pre) {
		return d.decode_fountain(m, sink, pre);
	};
}

int main(int argc, char** argv)
{
	cxxopts::Options options("cimbar encoder/decoder", "Demonstration program for cimbar codes");

	int compressionLevel = cimbar::Config::compression_level();
	unsigned ecc = cimbar::Config::ecc_bytes();
	options.add_options()
	    ("i,in", "Encoded pngs/jpgs/etc (for decode), or file to encode", cxxopts::value<vector<string>>())
	    ("o,out", "Output file or directory.", cxxopts::value<string>())
	    ("c,compression", "Compression level. 0 == no compression.", cxxopts::value<int>()->default_value(turbo::str::str(compressionLevel)))
	    ("e,ecc", "ECC level", cxxopts::value<unsigned>()->default_value(turbo::str::str(ecc)))
	    ("f,fountain", "Attempt fountain encode/decode", cxxopts::value<bool>())
	    ("encode", "Run the encoder!", cxxopts::value<bool>())
	    ("no-deskew", "Skip the deskew step -- treat input image as already extracted.", cxxopts::value<bool>())
	    ("undistort", "Attempt undistort step -- useful if image distortion is significant.", cxxopts::value<bool>())
	    ("preprocess", "Run sharpen filter on the input image. 1 == on. 0 == off. -1 == guess.", cxxopts::value<int>()->default_value("-1"))
	    ("h,help", "Print usage")
	;
	options.show_positional_help();
	options.parse_positional({"in"});
	options.positional_help("<in...>");

	auto result = options.parse(argc, argv);
	if (result.count("help") or !result.count("out") or !result.count("in"))
	{
	  std::cout << options.help() << std::endl;
	  exit(0);
	}

	vector<string> infiles = result["in"].as<vector<string>>();
	string outpath = result["out"].as<string>();

	bool encode = result.count("encode");
	bool fountain = result.count("fountain");
	compressionLevel = result["compression"].as<int>();
	ecc = result["ecc"].as<unsigned>();

	if (fountain)
		FountainInit::init();

	if (encode)
	{
		FileEncoder en(ecc);
		for (const string& f : infiles)
		{
			if (fountain)
				en.encode_fountain(f, outpath, compressionLevel);
			else
				en.encode(f, outpath);
		}
		return 0;
	}

	// else, decode
	bool no_deskew = result.count("no-deskew");
	bool undistort = result.count("undistort");
	int preprocess = result["preprocess"].as<int>();

	Decoder d(ecc);

	if (fountain)
	{
		unsigned chunkSize = cimbar::Config::fountain_chunk_size(ecc);
		if (compressionLevel <= 0)
		{
			fountain_decoder_sink<std::ofstream> sink(outpath, chunkSize);
			return decode(infiles, fountain_decode_fun(sink, d), no_deskew, undistort, preprocess);
		}

		// else -- default case, all bells and whistles
		fountain_decoder_sink<cimbar::zstd_decompressor<std::ofstream>> sink(outpath, chunkSize);
		return decode(infiles, fountain_decode_fun(sink, d), no_deskew, undistort, preprocess);
	}

	// else
	std::ofstream f(outpath);
	std::function<int(cv::UMat,bool)> fun = [&f, &d] (cv::UMat m, bool pre) {
		return d.decode(m, f, pre);
	};
	return decode(infiles, fun, no_deskew, undistort, preprocess);
}
