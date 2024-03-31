/**
 * @file pcmfile.c  Audio dumper using libpcmfile
 *
 * Copyright (C) 2024 Alfred E. Heggestad Vantablack
 */
#include <sndfile.h>
#include <time.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <stdio.h>
#include <curl/curl.h>

// #define URL    "127.0.0.1:8888/file?filename=%s/dump-test%d.pcm"

/**
 * @defgroup pcmfile pcmfile
 *
 * Audio filter that writes audio samples to WAV-file
 *
 * Example Configuration:
 \verbatim
  pcm_path 					/tmp/
 \endverbatim
 */


struct sndfile_enc {
	struct aufilt_enc_st af;  /* base class */
	SNDFILE *enc;
};

struct sndfile_dec {
	struct aufilt_dec_st af;  /* base class */
	enum aufmt fmt;
	FILE *file;
	int num;
};

static char file_path[512] = ".";
static char URL[512] = "127.0.0.1:8888/file?filename=%s/dump-test%d.pcm";
static CURL *curl = NULL;


static int timestamp_print(struct re_printf *pf, const struct tm *tm)
{
	if (!tm)
		return 0;

	return re_hprintf(pf, "%d-%02d-%02d-%02d-%02d-%02d",
			  1900 + tm->tm_year, tm->tm_mon + 1, tm->tm_mday,
			  tm->tm_hour, tm->tm_min, tm->tm_sec);
}


static void enc_destructor(void *arg)
{
	struct sndfile_enc *st = arg;

	if (st->enc)
		sf_close(st->enc);

	list_unlink(&st->af.le);
}


static void dec_destructor(void *arg)
{
	struct sndfile_dec *st = arg;
    	
	fclose(st->file);
	printf("close file");

	list_unlink(&st->af.le);
}


static int get_format(enum aufmt fmt)
{
	switch (fmt) {

	case AUFMT_S16LE:  return SF_FORMAT_PCM_16;
	case AUFMT_FLOAT:  return SF_FORMAT_FLOAT;
	default:           return 0;
	}
}


static SNDFILE *openfile(const struct aufilt_prm *prm,
			 const struct stream *strm,
			 bool enc)
{
	char filename[256];
	SF_INFO sfinfo;
	time_t tnow = time(0);
	struct tm *tm = localtime(&tnow);
	SNDFILE *sf;
	int format;

	const char *cname = stream_cname(strm);
	const char *peer = stream_peer(strm);

	(void)re_snprintf(filename, sizeof(filename),
			  "%s/dump-%s=foo>%s-%H-%s.wav",
			  file_path,
			  cname, peer,
			  timestamp_print, tm, enc ? "enc" : "dec");

	format = get_format(prm->fmt);
	if (!format) {
		warning("sndfile: sample format not supported (%s)\n",
			aufmt_name(prm->fmt));
		return NULL;
	}

	sfinfo.samplerate = prm->srate;
	sfinfo.channels   = prm->ch;
	sfinfo.format     = SF_FORMAT_WAV | format;

	sf = sf_open(filename, SFM_WRITE, &sfinfo);
	if (!sf) {
		warning("sndfile: could not open: %s\n", filename);
		puts(sf_strerror(NULL));
		return NULL;
	}

	info("sndfile: dumping %s audio to %s\n",
	     enc ? "encode" : "decode", filename);

	module_event("foo", "dump", NULL, NULL,
		     "%s/dump-%s=>%s-%H-%s.wav",
		     file_path,
		     cname, peer,
		     timestamp_print, tm, enc ? "enc" : "dec");

	return sf;
}


static int encode_update(struct aufilt_enc_st **stp, void **ctx,
			 const struct aufilt *af, struct aufilt_prm *prm,
			 const struct audio *au)
{
	struct sndfile_enc *st;
	const struct stream *strm = audio_strm(au);
	int err = 0;
	(void)ctx;
	(void)af;
	(void)au;

	if (!stp || !prm)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), enc_destructor);
	if (!st)
		return EINVAL;

	st->enc = openfile(prm, strm, true);
	if (!st->enc)
		err = ENOMEM;

	if (err)
		mem_deref(st);
	else
		*stp = (struct aufilt_enc_st *)st;

	return err;
}


static int decode_update(struct aufilt_dec_st **stp, void **ctx,
			 const struct aufilt *af, struct aufilt_prm *prm,
			 const struct audio *au)
{
	struct sndfile_dec *st;
//	const struct stream *strm = audio_strm(au);
	int err = 0;
	(void)ctx;
	(void)af;
	(void)au;

	if (!stp || !prm)
		return EINVAL;

	st = mem_zalloc(sizeof(*st), dec_destructor);
	if (!st)
        	return EINVAL;

	char filename[125]={0};


        (void)re_snprintf(filename, sizeof(filename),
                          "%s/dump-test%d.pcm", file_path, 0);

    	st->file=fopen(filename, "wb");
info("pcmfile: dumping %s \n", filename);
    	st->fmt = prm->fmt;
    	if( st->fmt == AUFMT_S16LE ){
        	info( "====ok  frequency and channel=%d=%d",prm->srate , prm->ch);
    	}

    	*stp = (struct aufilt_dec_st *)st;

    	return err;
}


static int encode(struct aufilt_enc_st *st, struct auframe *af)
{
	struct sndfile_enc *sf = (struct sndfile_enc *)st;
	size_t num_bytes;

	if (!st || !af)
		return EINVAL;

	num_bytes = auframe_size(af);

	sf_write_raw(sf->enc, af->sampv, num_bytes);

	return 0;
}


static int decode(struct aufilt_dec_st *st, struct auframe *af)
{
	struct sndfile_dec *sf = (struct sndfile_dec *)st;
    	size_t num_bytes;
 
    	if (!st || !af)
        	return EINVAL;
 
    	num_bytes = af->sampc * aufmt_sample_size(sf->fmt);
    	//printf("Decoding Writing");
	fwrite(af->sampv, af->sampc, aufmt_sample_size(sf->fmt) ,sf->file);
    	//info("decodeaudio=%d\n",  num_bytes);
	sf->num++;
	if (sf->num % 16 == 0) {
		info("file %d\n", sf->num);
		// new file
		if (sf->file) {
			fclose(sf->file);

			// send filename
			char url[255]={0};
			(void)re_snprintf(url, sizeof(url),
                          URL,
                          file_path,
                          (sf->num-1)/16);
			curl_easy_setopt(curl,CURLOPT_URL,url);
			curl_easy_perform(curl);
		}
		char filename[125]={0};


		(void)re_snprintf(filename, sizeof(filename),
                          "%s/dump-test%d.pcm",
                          file_path,
                          sf->num/16);

		//sprintf(filename, "test%d.pcm", st->num);
		sf->file = fopen(filename,"wb");
		
	}

	return 0;
}

//static struct aufilt sndfile = {  LE_INIT, "pcmfile", encode_update, encode, decodeaudio_update, decodeaudio };
static struct aufilt sndfile = {
	.name    = "pcmfile",
	.encupdh = encode_update,
	.ench    = encode,
	.decupdh = decode_update,
	.dech    = decode
};


static int module_init(void)
{
	aufilt_register(baresip_aufiltl(), &sndfile);

	conf_get_str(conf_cur(), "pcm_path", file_path, sizeof(file_path));
	conf_get_str(conf_cur(), "pcm_url", URL, sizeof(URL));

	info("pcmfile: saving files in %s\n", file_path);

	curl = curl_easy_init();
	if (!curl)
	{
		fprintf(stderr,"curl init failed\n");
		return -1;
	}

	return 0;
}


static int module_close(void)
{
	aufilt_unregister(&sndfile);
	curl_easy_cleanup(curl);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(sndfile) = {
	"pcmfile",
	"filter",
	module_init,
	module_close
};

