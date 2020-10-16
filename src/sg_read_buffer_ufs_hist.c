/*
 * Copyright (c) 2006-2019 Luben Tuikov and Douglas Gilbert.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the BSD_LICENSE file.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <ctype.h>
#include <string.h>
#include <getopt.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sg_lib.h"
#include "sg_cmds_basic.h"
#include "sg_cmds_extra.h"
#include "sg_pt.h"
#include "sg_unaligned.h"
#include "sg_pr2serr.h"

/*
 * This utility issues the SCSI READ BUFFER(10 or 16) command to the given
 * device.
 */

static const char * version_str = "1.27 20190113";      /* spc5r20 */


#ifndef SG_READ_BUFFER_10_CMD
#define SG_READ_BUFFER_10_CMD 0x3c
#define SG_READ_BUFFER_10_CMDLEN 10
#endif
#ifndef SG_READ_BUFFER_16_CMD
#define SG_READ_BUFFER_16_CMD 0x9b
#define SG_READ_BUFFER_16_CMDLEN 16
#endif

#define SENSE_BUFF_LEN  64       /* Arbitrary, could be larger */
#define DEF_PT_TIMEOUT  60       /* 60 seconds */


static struct option long_options[] = {
       	{"ufs_err", required_argument, 0, 'U'},
        {0, 0, 0, 0},   /* sentinel */
};


static void
usage()
{
    pr2serr("Usage: sg_read_buffer_ufs_hist DEVICE\n");
}


#define MODE_HEADER_DATA        0
#define MODE_VENDOR             1
#define MODE_DATA               2
#define MODE_DESCRIPTOR         3
#define MODE_ECHO_BUFFER        0x0A
#define MODE_ECHO_BDESC         0x0B
#define MODE_READ_MICROCODE_ST  0x0F
#define MODE_EN_EX_ECHO         0x1A
#define MODE_ERR_HISTORY        0x1C

static struct mode_s {
        const char *mode_string;
        int   mode;
        const char *comment;
} modes[] = {
        { "hd",         MODE_HEADER_DATA, "combined header and data"},
        { "vendor",     MODE_VENDOR,    "vendor specific"},
        { "data",       MODE_DATA,      "data"},
        { "desc",       MODE_DESCRIPTOR, "descriptor"},
        { "echo",       MODE_ECHO_BUFFER, "read data from echo buffer "
          "(spc-2)"},
        { "echo_desc",  MODE_ECHO_BDESC, "echo buffer descriptor (spc-2)"},
        { "rd_microc_st",  MODE_READ_MICROCODE_ST, "read microcode status "
          "(spc-5)"},
        { "en_ex",      MODE_EN_EX_ECHO,
          "enable expander communications protocol and echo buffer (spc-3)"},
        { "err_hist",   MODE_ERR_HISTORY, "error history (spc-4)"},
        { NULL,   999, NULL},   /* end sentinel */
};


static void
print_modes(void)
{
    const struct mode_s *mp;

    pr2serr("The modes parameter argument can be numeric (hex or decimal)\n"
            "or symbolic:\n");
    for (mp = modes; mp->mode_string; ++mp) {
        pr2serr(" %2d (0x%02x)  %-16s%s\n", mp->mode, mp->mode,
                mp->mode_string, mp->comment);
    }
}

/* Invokes a SCSI READ BUFFER(10) command (spc5r02).  Return of 0 -> success,
 * various SG_LIB_CAT_* positive values or -1 -> other errors */
static int
sg_ll_read_buffer_10(int sg_fd, int rb_mode, int rb_mode_sp, int rb_id,
                     uint32_t rb_offset, void * resp, int mx_resp_len,
                     int * residp, bool noisy, int verbose)
{
    int k, ret, res, sense_cat;
    uint8_t rb10_cb[SG_READ_BUFFER_10_CMDLEN] =
          {SG_READ_BUFFER_10_CMD, 0, 0, 0,  0, 0, 0, 0, 0, 0};
    uint8_t sense_b[SENSE_BUFF_LEN];
    struct sg_pt_base * ptvp;

    rb10_cb[1] = (uint8_t)(rb_mode & 0x1f);
    if (rb_mode_sp)
        rb10_cb[1] |= (uint8_t)((rb_mode_sp & 0x7) << 5);
    rb10_cb[2] = (uint8_t)rb_id;
    sg_put_unaligned_be24(rb_offset, rb10_cb + 3);
    sg_put_unaligned_be24(mx_resp_len, rb10_cb + 6);
    if (verbose) {
        pr2serr("    Read buffer(10) cdb: ");
        for (k = 0; k < SG_READ_BUFFER_10_CMDLEN; ++k)
            pr2serr("%02x ", rb10_cb[k]);
        pr2serr("\n");
    }

    ptvp = construct_scsi_pt_obj();
    if (NULL == ptvp) {
        pr2serr("Read buffer(10): out of memory\n");
        return -1;
    }
    set_scsi_pt_cdb(ptvp, rb10_cb, sizeof(rb10_cb));
    set_scsi_pt_sense(ptvp, sense_b, sizeof(sense_b));
    set_scsi_pt_data_in(ptvp, (uint8_t *)resp, mx_resp_len);
    res = do_scsi_pt(ptvp, sg_fd, DEF_PT_TIMEOUT, verbose);
    ret = sg_cmds_process_resp(ptvp, "Read buffer(10)", res, noisy, verbose,
                               &sense_cat);
    if (-1 == ret)
        ret = sg_convert_errno(get_scsi_pt_os_err(ptvp));
    else if (-2 == ret) {
        switch (sense_cat) {
        case SG_LIB_CAT_RECOVERED:
        case SG_LIB_CAT_NO_SENSE:
            ret = 0;
            break;
        default:
            ret = sense_cat;
            break;
        }
    } else {
        if ((verbose > 2) && (ret > 0)) {
            pr2serr("    Read buffer(10): response%s\n",
                    (ret > 256 ? ", first 256 bytes" : ""));
            hex2stderr((const uint8_t *)resp, (ret > 256 ? 256 : ret), -1);
        }
        ret = 0;
    }
    if (residp)
        *residp = get_scsi_pt_resid(ptvp);
    destruct_scsi_pt_obj(ptvp);
    return ret;
}

/* Invokes a SCSI READ BUFFER(16) command (spc5r02).  Return of 0 -> success,
 * various SG_LIB_CAT_* positive values or -1 -> other errors */
static int
sg_ll_read_buffer_16(int sg_fd, int rb_mode, int rb_mode_sp, int rb_id,
                     uint64_t rb_offset, void * resp, int mx_resp_len,
                     int * residp, bool noisy, int verbose)
{
    int k, ret, res, sense_cat;
    uint8_t rb16_cb[SG_READ_BUFFER_16_CMDLEN] =
          {SG_READ_BUFFER_16_CMD, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
           0, 0, 0, 0};
    uint8_t sense_b[SENSE_BUFF_LEN];
    struct sg_pt_base * ptvp;

    rb16_cb[1] = (uint8_t)(rb_mode & 0x1f);
    if (rb_mode_sp)
        rb16_cb[1] |= (uint8_t)((rb_mode_sp & 0x7) << 5);
    sg_put_unaligned_be64(rb_offset, rb16_cb + 2);
    sg_put_unaligned_be24(mx_resp_len, rb16_cb + 11);
    rb16_cb[14] = (uint8_t)rb_id;
    if (verbose) {
        pr2serr("    Read buffer(16) cdb: ");
        for (k = 0; k < SG_READ_BUFFER_16_CMDLEN; ++k)
            pr2serr("%02x ", rb16_cb[k]);
        pr2serr("\n");
    }

    ptvp = construct_scsi_pt_obj();
    if (NULL == ptvp) {
        pr2serr("Read buffer(16): out of memory\n");
        return -1;
    }
    set_scsi_pt_cdb(ptvp, rb16_cb, sizeof(rb16_cb));
    set_scsi_pt_sense(ptvp, sense_b, sizeof(sense_b));
    set_scsi_pt_data_in(ptvp, (uint8_t *)resp, mx_resp_len);
    res = do_scsi_pt(ptvp, sg_fd, DEF_PT_TIMEOUT, verbose);
    ret = sg_cmds_process_resp(ptvp, "Read buffer(16)", res, noisy, verbose,
                               &sense_cat);
    if (-1 == ret)
        ret = sg_convert_errno(get_scsi_pt_os_err(ptvp));
    else if (-2 == ret) {
        switch (sense_cat) {
        case SG_LIB_CAT_RECOVERED:
        case SG_LIB_CAT_NO_SENSE:
            ret = 0;
            break;
        default:
            ret = sense_cat;
            break;
        }
    } else {
        if ((verbose > 2) && (ret > 0)) {
            pr2serr("    Read buffer(16): response%s\n",
                    (ret > 256 ? ", first 256 bytes" : ""));
            hex2stderr((const uint8_t *)resp, (ret > 256 ? 256 : ret), -1);
        }
        ret = 0;
    }
    if (residp)
        *residp = get_scsi_pt_resid(ptvp);
    destruct_scsi_pt_obj(ptvp);
    return ret;
}

static void
dStrRaw(const uint8_t * str, int len)
{
    int k;

    for (k = 0; k < len; ++k)
        printf("%c", str[k]);
}

/* UFS 3.0 Error History */
struct eh_directory_entry {
	uint8_t buffer_id;
	uint8_t reserved[3];
	uint32_t max_available_length;
};

struct eh_directory_header {
	uint8_t vendor_id[8];
	uint8_t version;
	uint8_t reserved1;
	uint8_t reserved2[20];
	uint16_t directory_length;
};

#define EH_BUFFER_MODE 				0x1C 	// Error history
#define EH_DIR_BUFFER_MAX			0xFFFFFF
#define EH_BUFFER_LEN				(2088)
#define EH_BUFFER_ID_MIN		0x10
#define EH_BUFFER_ID_MAX		0xEF
#define EH_ERR_DATA_BUF_SIZE		(256*1024)


#define DIR_FILENAME			"err_directory.dat"
#define HIST_FILENAME_POSTFIX	"err_history.dat"

static int do_read_buffer(int sg_fd, int rb_mode, int rb_mode_sp, int rb_id,
                     uint32_t rb_offset, void * resp, int mx_resp_len,
                     int * residp, bool noisy, int verbose)
{
	int res;
	res = sg_ll_read_buffer_10(sg_fd, rb_mode, rb_mode_sp, rb_id,
                                   (uint32_t)rb_offset, resp, mx_resp_len, residp,
                                   noisy, verbose);
	if (0 != res) {
        char b[80];

        if (res > 0) {
            sg_get_category_sense_str(res, sizeof(b), b, verbose);
            pr2serr("Read buffer failed: %s\n", b);
        }
    }

	return res;
}

static int do_ufs_error_history(int sg_fd)
{
	int len;
	int err;
	uint8_t * dir_header_buf;
	struct eh_directory_header *p_eh_dir_header = NULL;
	struct eh_directory_entry *p_eh_dir_entry = NULL;
	int dir_fd, history_fd;
	uint16_t directory_length;
	uint16_t i;
	uint8_t * err_data_buf = NULL;
	char err_dir_filename[256] = {0,};

	len = EH_BUFFER_LEN;
    dir_header_buf = (uint8_t *)malloc(len);
    if (NULL == dir_header_buf) {
        pr2serr("unable to allocate %d bytes on the heap\n", len);
        return SG_LIB_CAT_OTHER;
    }
    memset(dir_header_buf, 0, len);

	pr2serr("Reading header for error history\n");
	err = do_read_buffer(sg_fd, EH_BUFFER_MODE, 0, 0, 0, dir_header_buf, EH_BUFFER_LEN, 0, false, 0);

	if (err) {
		pr2serr("Read history directory failed\n");
		goto out_free;
	}

	dir_fd = open(DIR_FILENAME, O_CREAT|O_WRONLY, 0644);
	if (!dir_fd) {
		pr2serr("Open %s failed\n", DIR_FILENAME);
		err = -1;
		goto out_free;
	}

	write(dir_fd, dir_header_buf, len);
	close(dir_fd);
	pr2serr("Saved error history directory to %s\n", DIR_FILENAME);

	p_eh_dir_header = (struct eh_directory_header *)dir_header_buf;
	directory_length = be16toh(p_eh_dir_header->directory_length);
	pr2serr("Directory length : %u\n", directory_length);

	// initialize entry pointer
	p_eh_dir_entry = (struct eh_directory_entry *)(dir_header_buf + sizeof(struct eh_directory_header));
	err_data_buf = (uint8_t *)malloc(EH_ERR_DATA_BUF_SIZE);
	if (!err_data_buf) {
		pr2serr("unable to allocate %d bytes on the heap\n", len);
		goto out_free;
	}

	// read history buffer per directory buffer id
	for ( i=0; i < (directory_length/sizeof(struct eh_directory_entry)); ++i) {
		uint32_t max_available_length;
		uint32_t read_sum = 0;

		// per directory entry process
		max_available_length = be32toh(p_eh_dir_entry->max_available_length);

		if ( (p_eh_dir_entry->buffer_id < EH_BUFFER_ID_MIN || p_eh_dir_entry->buffer_id > EH_BUFFER_ID_MAX) ||
			 (max_available_length > EH_DIR_BUFFER_MAX || max_available_length==0) ) {
			//pr2serr("invalid buffer id or length - skip\n");
			p_eh_dir_entry = p_eh_dir_entry + 1;
			continue;
		}
		pr2serr("UFS ERROR_BUFFER_ID : %u, max_available_length(%u)\n", p_eh_dir_entry->buffer_id, max_available_length);

		sprintf(err_dir_filename, "%d_%s", p_eh_dir_entry->buffer_id, HIST_FILENAME_POSTFIX);
		history_fd = open(err_dir_filename, O_CREAT|O_WRONLY, 0644);
		if (!history_fd) {
			pr2serr("Open %s failed\n", err_dir_filename);
			p_eh_dir_entry = p_eh_dir_entry + 1;
			continue;
		}

		while (read_sum < max_available_length) {

			int size;

			memset(err_data_buf, 0x0, EH_ERR_DATA_BUF_SIZE);
			if ((max_available_length-read_sum)>EH_ERR_DATA_BUF_SIZE)
				size = EH_ERR_DATA_BUF_SIZE;
			else
				size = max_available_length-read_sum;

			err = do_read_buffer(sg_fd, EH_BUFFER_MODE, 0, p_eh_dir_entry->buffer_id, read_sum, err_data_buf, size, 0, 0, 0);

			if (err) {
				pr2serr("Read error history buffer failed : id(%u)\n", p_eh_dir_entry->buffer_id);
				break;
			}
			write(history_fd, err_data_buf, size);

			read_sum += size;
		}
		close(history_fd);
		pr2serr("Saved error history buffer for id(%u) to %s\n", p_eh_dir_entry->buffer_id, err_dir_filename);

		p_eh_dir_entry = p_eh_dir_entry + 1;
	}

out_free:
	if (dir_header_buf) {
		free(dir_header_buf);
		dir_header_buf = NULL;
	}
	if (err_data_buf) {
		free(err_data_buf);
		err_data_buf = NULL;
	}
	return err;
}


int
main(int argc, char * argv[])
{
	bool o_readonly = false;
	bool do_raw = false;
    int res, c;
    int sg_fd = -1;
    int rb_len = 4;
    int resid = 0;
	int verbose = 0;
    int ret = 0;
    const char * device_name = NULL;
	int do_ufs_err = 1;

    while (1) {
        int option_index = 0;

        c = getopt_long(argc, argv, "U", long_options,
                        &option_index);
        if (c == -1)
            break;

        switch (c) {

		case 'U':
			do_ufs_err = true;
			break;
        default:
            pr2serr("unrecognised option code 0x%x ??\n", c);
            usage();
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    if (optind < argc) {
        if (NULL == device_name) {
            device_name = argv[optind];
            ++optind;
        }
        if (optind < argc) {
            for (; optind < argc; ++optind)
                pr2serr("Unexpected extra argument: %s\n", argv[optind]);
            usage();
            return SG_LIB_SYNTAX_ERROR;
        }
    }

    if (NULL == device_name) {
        pr2serr("Missing device name!\n\n");
        usage();
        return SG_LIB_SYNTAX_ERROR;
    }

	do_raw = true;
	if (do_raw) {
        if (sg_set_binary_mode(STDOUT_FILENO) < 0) {
            perror("sg_set_binary_mode");
            ret = SG_LIB_FILE_ERROR;
            goto fini;
        }
    }

    sg_fd = sg_cmds_open_device(device_name, o_readonly, verbose);
    if (sg_fd < 0) {
        if (verbose)
            pr2serr("open error: %s: %s\n", device_name,
                    safe_strerror(-sg_fd));
        ret = sg_convert_errno(-sg_fd);
        goto fini;
    }

	if (do_ufs_err) {
		res = do_ufs_error_history(sg_fd);
    }

    if (resid > 0)
        rb_len -= resid;        /* got back less than requested */
    if (rb_len > 0) {
        if (do_raw) {
            //dStrRaw(resp, rb_len);
       	}
     }

fini:
    if (sg_fd >= 0) {
        res = sg_cmds_close_device(sg_fd);
        if (res < 0) {
            pr2serr("close error: %s\n", safe_strerror(-res));
            if (0 == ret)
                ret = sg_convert_errno(-res);
        }
    }
    if (0 == verbose) {
        if (! sg_if_can2stderr("sg_read_buffer failed: ", ret))
            pr2serr("Some error occurred, try again with '-v' "
                    "or '-vv' for more information\n");
    }
    return (ret >= 0) ? ret : SG_LIB_CAT_OTHER;
}
