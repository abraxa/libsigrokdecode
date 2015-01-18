/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "libsigrokdecode-internal.h" /* First, so we avoid a _POSIX_C_SOURCE warning. */
#include "libsigrokdecode.h"
#include "config.h"
#include <inttypes.h>

typedef struct {
        PyObject_HEAD
} srd_Decoder;

/* This is only used for nicer srd_dbg() output. */
static const char *OUTPUT_TYPES[] = {
	"OUTPUT_ANN",
	"OUTPUT_PYTHON",
	"OUTPUT_BINARY",
	"OUTPUT_META",
	"OUTPUT_PACKET",
};

static int convert_annotation(struct srd_decoder_inst *di, PyObject *obj,
		struct srd_proto_data *pdata)
{
	PyObject *py_tmp;
	struct srd_pd_output *pdo;
	struct srd_proto_data_annotation *pda;
	int ann_class;
	char **ann_text;

	/* Should be a list of [annotation class, [string, ...]]. */
	if (!PyList_Check(obj) && !PyTuple_Check(obj)) {
		srd_err("Protocol decoder %s submitted %s instead of list.",
			di->decoder->name, obj->ob_type->tp_name);
		return SRD_ERR_PYTHON;
	}

	/* Should have 2 elements. */
	if (PyList_Size(obj) != 2) {
		srd_err("Protocol decoder %s submitted annotation list with "
			"%d elements instead of 2", di->decoder->name,
			PyList_Size(obj));
		return SRD_ERR_PYTHON;
	}

	/*
	 * The first element should be an integer matching a previously
	 * registered annotation class.
	 */
	py_tmp = PyList_GetItem(obj, 0);
	if (!PyLong_Check(py_tmp)) {
		srd_err("Protocol decoder %s submitted annotation list, but "
			"first element was not an integer.", di->decoder->name);
		return SRD_ERR_PYTHON;
	}
	ann_class = PyLong_AsLong(py_tmp);
	if (!(pdo = g_slist_nth_data(di->decoder->annotations, ann_class))) {
		srd_err("Protocol decoder %s submitted data to unregistered "
			"annotation class %d.", di->decoder->name, ann_class);
		return SRD_ERR_PYTHON;
	}

	/* Second element must be a list. */
	py_tmp = PyList_GetItem(obj, 1);
	if (!PyList_Check(py_tmp)) {
		srd_err("Protocol decoder %s submitted annotation list, but "
			"second element was not a list.", di->decoder->name);
		return SRD_ERR_PYTHON;
	}
	if (py_strseq_to_char(py_tmp, &ann_text) != SRD_OK) {
		srd_err("Protocol decoder %s submitted annotation list, but "
			"second element was malformed.", di->decoder->name);
		return SRD_ERR_PYTHON;
	}

	if (!(pda = g_try_malloc(sizeof(struct srd_proto_data_annotation))))
		return SRD_ERR_MALLOC;
	pda->ann_class = ann_class;
	pda->ann_text = ann_text;
	pdata->data = pda;

	return SRD_OK;
}

static int convert_binary(struct srd_decoder_inst *di, PyObject *obj,
		struct srd_proto_data *pdata)
{
	struct srd_proto_data_binary *pdb;
	PyObject *py_tmp;
	Py_ssize_t size;
	int bin_class;
	char *class_name, *buf;

	/* Should be a tuple of (binary class, bytes). */
	if (!PyTuple_Check(obj)) {
		srd_err("Protocol decoder %s submitted SRD_OUTPUT_BINARY with "
				"%s instead of tuple.", di->decoder->name,
				obj->ob_type->tp_name);
		return SRD_ERR_PYTHON;
	}

	/* Should have 2 elements. */
	if (PyTuple_Size(obj) != 2) {
		srd_err("Protocol decoder %s submitted SRD_OUTPUT_BINARY tuple "
				"with %d elements instead of 2", di->decoder->name,
				PyList_Size(obj));
		return SRD_ERR_PYTHON;
	}

	/* The first element should be an integer. */
	py_tmp = PyTuple_GetItem(obj, 0);
	if (!PyLong_Check(py_tmp)) {
		srd_err("Protocol decoder %s submitted SRD_OUTPUT_BINARY tuple, "
			"but first element was not an integer.", di->decoder->name);
		return SRD_ERR_PYTHON;
	}
	bin_class = PyLong_AsLong(py_tmp);
	if (!(class_name = g_slist_nth_data(di->decoder->binary, bin_class))) {
		srd_err("Protocol decoder %s submitted SRD_OUTPUT_BINARY with "
			"unregistered binary class %d.", di->decoder->name, bin_class);
		return SRD_ERR_PYTHON;
	}

	/* Second element should be bytes. */
	py_tmp = PyTuple_GetItem(obj, 1);
	if (!PyBytes_Check(py_tmp)) {
		srd_err("Protocol decoder %s submitted SRD_OUTPUT_BINARY tuple, "
			"but second element was not bytes.", di->decoder->name);
		return SRD_ERR_PYTHON;
	}

	/* Consider an empty set of bytes a bug. */
	if (PyBytes_Size(py_tmp) == 0) {
		srd_err("Protocol decoder %s submitted SRD_OUTPUT_BINARY "
				"with empty data set.", di->decoder->name);
		return SRD_ERR_PYTHON;
	}

	if (!(pdb = g_try_malloc(sizeof(struct srd_proto_data_binary))))
		return SRD_ERR_MALLOC;
	if (PyBytes_AsStringAndSize(py_tmp, &buf, &size) == -1)
		return SRD_ERR_PYTHON;
	pdb->bin_class = bin_class;
	pdb->size = size;
	if (!(pdb->data = g_try_malloc(pdb->size)))
		return SRD_ERR_MALLOC;
	memcpy((void *)pdb->data, (const void *)buf, pdb->size);
	pdata->data = pdb;

	return SRD_OK;
}

static int convert_meta(struct srd_proto_data *pdata, PyObject *obj)
{
	long long intvalue;
	double dvalue;

	if (pdata->pdo->meta_type == G_VARIANT_TYPE_INT64) {
		if (!PyLong_Check(obj)) {
			PyErr_Format(PyExc_TypeError, "This output was registered "
					"as 'int', but '%s' was passed.", obj->ob_type->tp_name);
			return SRD_ERR_PYTHON;
		}
		intvalue = PyLong_AsLongLong(obj);
		if (PyErr_Occurred())
			return SRD_ERR_PYTHON;
		pdata->data = g_variant_new_int64(intvalue);
	} else if (pdata->pdo->meta_type == G_VARIANT_TYPE_DOUBLE) {
		if (!PyFloat_Check(obj)) {
			PyErr_Format(PyExc_TypeError, "This output was registered "
					"as 'float', but '%s' was passed.", obj->ob_type->tp_name);
			return SRD_ERR_PYTHON;
		}
		dvalue = PyFloat_AsDouble(obj);
		if (PyErr_Occurred())
			return SRD_ERR_PYTHON;
		pdata->data = g_variant_new_double(dvalue);
	}

	return SRD_OK;
}

static int convert_packet(struct srd_decoder_inst *di, struct srd_proto_data *pdata, PyObject *obj)
{
	PyObject *py_tmp;
	struct srd_pd_output *pdo;
	struct srd_proto_data_packet *pdp;
	int ann_class, packet_output_type;

	int packet_num = 0;
	char *field_name = NULL;
	char *field_value = NULL;

	/* Allowed parameters (start sample and end sample are already evaluated):
	 * SRD_PACKET_LOCATION: [annotation class, packet output type, packet num]
	 * SRD_PACKET_FIELD:    [annotation class, packet output type, packet num,
	 *                       field name, field value]
	 */

	/* Should be a list. */
	if (!PyList_Check(obj)) {
		srd_err("Protocol decoder %s submitted %s instead of list.",
			di->decoder->name, obj->ob_type->tp_name);
		return SRD_ERR_PYTHON;
	}

	/*
	 * The first element should be an integer matching a previously
	 * registered annotation class.
	 */
	py_tmp = PyList_GetItem(obj, 0);
	if (!PyLong_Check(py_tmp)) {
		srd_err("Protocol decoder %s submitted packet output list, but "
			"first element was not an integer.", di->decoder->name);
		return SRD_ERR_PYTHON;
	}
	ann_class = PyLong_AsLong(py_tmp);
	if (!(pdo = g_slist_nth_data(di->decoder->annotations, ann_class))) {
		srd_err("Protocol decoder %s submitted data to unregistered "
			"annotation class %d.", di->decoder->name, ann_class);
		return SRD_ERR_PYTHON;
	}

	/* Second element must be a packet output type. */
	py_tmp = PyList_GetItem(obj, 1);
	if (!PyLong_Check(py_tmp)) {
		srd_err("Protocol decoder %s submitted packet output list, but "
			"second element was not an int.", di->decoder->name);
		return SRD_ERR_PYTHON;
	}
	packet_output_type = PyLong_AsLong(py_tmp);

	switch (packet_output_type) {
	case SRD_PACKET_LOCATION:
		/* List should have 3 elements. */
		if (PyList_Size(obj) != 3) {
			srd_err("Protocol decoder %s submitted SRD_PACKET_LOCATION list with "
				"%d elements instead of 3", di->decoder->name,
				PyList_Size(obj));
			return SRD_ERR_PYTHON;
		}

		py_tmp = PyList_GetItem(obj, 2);
		if (!PyLong_Check(py_tmp)) {
			srd_err("Protocol decoder %s submitted SRD_PACKET_LOCATION list, but "
				"third element was not an int.", di->decoder->name);
			return SRD_ERR_PYTHON;
		}
		packet_num = PyLong_AsLong(py_tmp);
		break;
	case SRD_PACKET_FIELD:
		/* List should have 5 elements. */
		if (PyList_Size(obj) != 5) {
			srd_err("Protocol decoder %s submitted SRD_PACKET_FIELD list with "
				"%d elements instead of 5", di->decoder->name,
				PyList_Size(obj));
			return SRD_ERR_PYTHON;
		}

		py_tmp = PyList_GetItem(obj, 2);
		if (!PyLong_Check(py_tmp)) {
			srd_err("Protocol decoder %s submitted SRD_PACKET_FIELD list, but "
				"third element was not an int.", di->decoder->name);
			return SRD_ERR_PYTHON;
		}
		packet_num = PyLong_AsLong(py_tmp);

		py_tmp = PyList_GetItem(obj, 3);
		if (!PyUnicode_Check(py_tmp)) {
			srd_err("Protocol decoder %s submitted SRD_PACKET_FIELD list, but "
				"fourth element was not a string.", di->decoder->name);
			return SRD_ERR_PYTHON;
		}
		py_tmp = PyUnicode_AsASCIIString(py_tmp);
		field_name = g_strdup(PyBytes_AsString(py_tmp));
		Py_XDECREF(py_tmp);

		py_tmp = PyList_GetItem(obj, 4);
		if (!PyUnicode_Check(py_tmp)) {
			srd_err("Protocol decoder %s submitted SRD_PACKET_FIELD list, but "
				"fifth element was not a string.", di->decoder->name);
			return SRD_ERR_PYTHON;
		}
		py_tmp = PyUnicode_AsASCIIString(py_tmp);
		field_value = g_strdup(PyBytes_AsString(py_tmp));
		Py_XDECREF(py_tmp);
		break;
	default:
		srd_err("Protocol decoder %s submitted invalid sub type %d for "
			"SRD_OUTPUT_PACKET output type.", di->decoder->name,
			packet_output_type);
		return SRD_ERR_PYTHON;
	}

	pdp = g_malloc(sizeof(struct srd_proto_data_packet));
	pdp->ann_class = ann_class;
	pdp->packet_output_type = packet_output_type;
	pdp->packet_num = packet_num;
	pdp->field_name = field_name;
	pdp->field_value = field_value;

	pdata->data = pdp;

	return SRD_OK;
}

static PyObject *Decoder_put(PyObject *self, PyObject *args)
{
	GSList *l;
	PyObject *py_data, *py_res;
	struct srd_decoder_inst *di, *next_di;
	struct srd_pd_output *pdo;
	struct srd_proto_data *pdata;
	uint64_t start_sample, end_sample;
	int output_id;
	struct srd_pd_callback *cb;

	if (!(di = srd_inst_find_by_obj(NULL, self))) {
		/* Shouldn't happen. */
		srd_dbg("put(): self instance not found.");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "KKiO", &start_sample, &end_sample,
		&output_id, &py_data)) {
		/*
		 * This throws an exception, but by returning NULL here we let
		 * Python raise it. This results in a much better trace in
		 * controller.c on the decode() method call.
		 */
		return NULL;
	}

	if (!(l = g_slist_nth(di->pd_output, output_id))) {
		srd_err("Protocol decoder %s submitted invalid output ID %d.",
			di->decoder->name, output_id);
		return NULL;
	}
	pdo = l->data;

	srd_spew("Instance %s put %" PRIu64 "-%" PRIu64 " %s on oid %d.",
		 di->inst_id, start_sample, end_sample,
		 OUTPUT_TYPES[pdo->output_type], output_id);

	if (!(pdata = g_try_malloc0(sizeof(struct srd_proto_data)))) {
		srd_err("Failed to g_malloc() struct srd_proto_data.");
		return NULL;
	}
	pdata->start_sample = start_sample;
	pdata->end_sample = end_sample;
	pdata->pdo = pdo;

	switch (pdo->output_type) {
	case SRD_OUTPUT_ANN:
		/* Annotations are only fed to callbacks. */
		if ((cb = srd_pd_output_callback_find(di->sess, pdo->output_type))) {
			/* Convert from PyDict to srd_proto_data_annotation. */
			if (convert_annotation(di, py_data, pdata) != SRD_OK) {
				/* An error was already logged. */
				break;
			}
			cb->cb(pdata, cb->cb_data);
		}
		break;
	case SRD_OUTPUT_PYTHON:
		for (l = di->next_di; l; l = l->next) {
			next_di = l->data;
			srd_spew("Sending %d-%d to instance %s",
				 start_sample, end_sample, next_di->inst_id);
			if (!(py_res = PyObject_CallMethod(
				next_di->py_inst, "decode", "KKO", start_sample,
				end_sample, py_data))) {
				srd_exception_catch("Calling %s decode(): ",
							next_di->inst_id);
			}
			Py_XDECREF(py_res);
		}
		if ((cb = srd_pd_output_callback_find(di->sess, pdo->output_type))) {
			/* Frontends aren't really supposed to get Python
			 * callbacks, but it's useful for testing. */
			pdata->data = py_data;
			cb->cb(pdata, cb->cb_data);
		}
		break;
	case SRD_OUTPUT_BINARY:
		if ((cb = srd_pd_output_callback_find(di->sess, pdo->output_type))) {
			/* Convert from PyDict to srd_proto_data_binary. */
			if (convert_binary(di, py_data, pdata) != SRD_OK) {
				/* An error was already logged. */
				break;
			}
			cb->cb(pdata, cb->cb_data);
		}
		break;
	case SRD_OUTPUT_META:
		if ((cb = srd_pd_output_callback_find(di->sess, pdo->output_type))) {
			/* Annotations need converting from PyObject. */
			if (convert_meta(pdata, py_data) != SRD_OK) {
				/* An exception was already set up. */
				break;
			}
			cb->cb(pdata, cb->cb_data);
		}
		break;
	case SRD_OUTPUT_PACKET:
		/* Packets go up the decoder stack and are also sent to the frontend. */
		for (l = di->next_di; l; l = l->next) {
			next_di = l->data;
			srd_spew("Sending packet %d-%d to instance %s",
					start_sample, end_sample, next_di->inst_id);
			if (!(py_res = PyObject_CallMethod(
				next_di->py_inst, "decode", "KKO", start_sample,
				end_sample, py_data))) {
				srd_exception_catch("Calling %s decode(): ",
						next_di->inst_id);
			}
			Py_XDECREF(py_res);
		}

		if ((cb = srd_pd_output_callback_find(di->sess, pdo->output_type))) {
			/* Packets need converting from PyObject. */
			if (convert_packet(di, pdata, py_data) != SRD_OK) {
				/* An exception was already set up. */
				break;
			}
			cb->cb(pdata, cb->cb_data);
		}
		break;
	default:
		srd_err("Protocol decoder %s submitted invalid output type %d.",
			di->decoder->name, pdo->output_type);
		break;
	}

	g_free(pdata);

	Py_RETURN_NONE;
}

static PyObject *Decoder_register(PyObject *self, PyObject *args,
		PyObject *kwargs)
{
	struct srd_decoder_inst *di;
	struct srd_pd_output *pdo;
	PyObject *py_new_output_id;
	PyTypeObject *meta_type_py;
	const GVariantType *meta_type_gv;
	int output_type;
	char *proto_id, *meta_name, *meta_descr;
	char *keywords[] = {"output_type", "proto_id", "meta", NULL};

	meta_type_py = NULL;
	meta_type_gv = NULL;
	meta_name = meta_descr = NULL;

	if (!(di = srd_inst_find_by_obj(NULL, self))) {
		PyErr_SetString(PyExc_Exception, "decoder instance not found");
		return NULL;
	}

	/* Default to instance id, which defaults to class id. */
	proto_id = di->inst_id;
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "i|s(Oss)", keywords,
			&output_type, &proto_id,
			&meta_type_py, &meta_name, &meta_descr)) {
		/* Let Python raise this exception. */
		return NULL;
	}

	/* Check if the meta value's type is supported. */
	if (output_type == SRD_OUTPUT_META) {
		if (meta_type_py == &PyLong_Type)
			meta_type_gv = G_VARIANT_TYPE_INT64;
		else if (meta_type_py == &PyFloat_Type)
			meta_type_gv = G_VARIANT_TYPE_DOUBLE;
		else {
			PyErr_Format(PyExc_TypeError, "Unsupported type '%s'.",
					meta_type_py->tp_name);
			return NULL;
		}
	}

	srd_dbg("Instance %s creating new output type %d for %s.",
		di->inst_id, output_type, proto_id);

	if (!(pdo = g_try_malloc(sizeof(struct srd_pd_output)))) {
		PyErr_SetString(PyExc_MemoryError, "struct srd_pd_output");
		return NULL;
	}

	/* pdo_id is just a simple index, nothing is deleted from this list anyway. */
	pdo->pdo_id = g_slist_length(di->pd_output);
	pdo->output_type = output_type;
	pdo->di = di;
	pdo->proto_id = g_strdup(proto_id);

	if (output_type == SRD_OUTPUT_META) {
		pdo->meta_type = meta_type_gv;
		pdo->meta_name = g_strdup(meta_name);
		pdo->meta_descr = g_strdup(meta_descr);
	}

	di->pd_output = g_slist_append(di->pd_output, pdo);
	py_new_output_id = Py_BuildValue("i", pdo->pdo_id);

	return py_new_output_id;
}

static PyMethodDef Decoder_methods[] = {
	{"put", Decoder_put, METH_VARARGS,
	 "Accepts a dictionary with the following keys: startsample, endsample, data"},
	{"register", (PyCFunction)Decoder_register, METH_VARARGS|METH_KEYWORDS,
			"Register a new output stream"},
	{NULL, NULL, 0, NULL}
};

/** @cond PRIVATE */
SRD_PRIV PyTypeObject srd_Decoder_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "sigrokdecode.Decoder",
	.tp_basicsize = sizeof(srd_Decoder),
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_doc = "sigrok Decoder base class",
	.tp_methods = Decoder_methods,
};
/** @endcond */
