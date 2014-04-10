#include "capture_filter/resize_utils.h"

using namespace cv;

int resize_frame(char *indata, codec_t in_color, char *outdata, unsigned int *data_len, unsigned int width, unsigned int height, double scale_factor){
    int res = 0;
    Mat out, in, rgb;
    assert(in_color == UYVY);
    if (indata == NULL || outdata == NULL || data_len == NULL) {
        return 1;
    }

    in.create(height, width, CV_8UC2);
    in.data = (uchar*)indata;

	cvtColor(in, rgb, CV_YUV2RGB_UYVY);
	resize(rgb, out, Size(0,0), scale_factor, scale_factor, INTER_LINEAR);

    *data_len = out.step * out.rows * sizeof(char);
    memcpy(outdata,out.data,*data_len);

    return res;
}
