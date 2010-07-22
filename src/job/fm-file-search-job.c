#include "fm-file-search-job.h"

#include <gio/gio.h> /* for GFile, GFileInfo, GFileEnumerator */
#include <string.h> /* for strstr */
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#define _GNU_SOURCE 1
#include <fnmatch.h>

#include "fm-list.h"

extern const char gfile_info_query_attribs[]; /* defined in fm-file-info-job.c */

static void fm_file_search_job_finalize(GObject * object);
gboolean fm_file_search_job_run(FmFileSearchJob* job);

G_DEFINE_TYPE(FmFileSearchJob, fm_file_search_job, FM_TYPE_JOB);

static void for_each_target_folder(GFile * path, FmFileSearchJob * job);
static gboolean run_rules_for_each_file_info(GFileInfo * info, GFile * parent, FmFileSearchJob * job);
static void load_target_folders(FmPath * path, gpointer user_data);

static gboolean file_content_search_ginputstream(FmFileSearchFuncData * data);
static gboolean file_content_search_mmap(FmFileSearchFuncData * data);
static gboolean content_search(char * haystack, FmFileSearchFuncData * data);

static void fm_file_search_job_class_init(FmFileSearchJobClass * klass)
{
	GObjectClass * g_object_class;
	FmJobClass * job_class = FM_JOB_CLASS(klass);
	g_object_class = G_OBJECT_CLASS(klass);
	g_object_class->finalize = fm_file_search_job_finalize;

	job_class->run = fm_file_search_job_run;
}

static void fm_file_search_job_init(FmFileSearchJob * self)
{
    fm_job_init_cancellable(FM_JOB(self));
}

static void fm_file_search_job_finalize(GObject * object)
{
	FmFileSearchJob *self;

	g_return_if_fail(object != NULL);
	g_return_if_fail(FM_IS_FILE_SEARCH_JOB(object));

	self = FM_FILE_SEARCH_JOB(object);

	if(self->files)
		fm_list_unref(self->files);

	if(self->rules)
		g_slist_free(self->rules);

	if(self->target_folders)
		g_slist_free(self->target_folders);

	if(self->settings)
		g_slice_free(FmFileSearchSettings, self->settings);

	if(self->target_regex)
		g_regex_unref(self->target_regex);


	if(self->target_contains_regex)
		g_regex_unref(self->target_contains_regex);

	if (G_OBJECT_CLASS(fm_file_search_job_parent_class)->finalize)
		(* G_OBJECT_CLASS(fm_file_search_job_parent_class)->finalize)(object);
}

FmJob * fm_file_search_job_new(GSList * rules, FmPathList * target_folders, FmFileSearchSettings * settings)
{
	FmFileSearchJob * job = (FmJob*)g_object_new(FM_TYPE_FILE_SEARCH_JOB, NULL);

	job->files = fm_file_info_list_new();

	job->rules = g_slist_copy(rules);

	job->target_folders = NULL;
	fm_list_foreach(target_folders, load_target_folders, job);

	job->settings = g_slice_dup(FmFileSearchSettings, settings); 

	if(job->settings->target_mode == FM_FILE_SEARCH_MODE_REGEX && job->settings->target != NULL)
	{
		if(job->settings->case_sensitive)
			job->target_regex = g_regex_new(job->settings->target, G_REGEX_OPTIMIZE, 0, NULL);
		else
			job->target_regex = g_regex_new(job->settings->target, G_REGEX_OPTIMIZE | G_REGEX_CASELESS, 0, NULL);
	}

	if(job->settings->content_mode == FM_FILE_SEARCH_MODE_REGEX && job->settings->target_contains != NULL)
	{
		if(job->settings->case_sensitive)
			job->target_contains_regex = g_regex_new(job->settings->target_contains, G_REGEX_OPTIMIZE, 0, NULL);
		else
			job->target_contains_regex = g_regex_new(job->settings->target_contains, G_REGEX_OPTIMIZE | G_REGEX_CASELESS, 0, NULL);
	}

	return (FmJob*)job;
}

gboolean fm_file_search_job_run(FmFileSearchJob * job)
{
	/* TODO: error handling (what sort of errors could occur?) */	

	GSList * folders = job->target_folders;

	while(folders != NULL && !fm_job_is_cancelled(FM_JOB(job)))
	{
		GFile * path = G_FILE(folders->data); //each one should represent a directory
		/* emit error if one is not a directory */
		for_each_target_folder(path, job);
		folders = folders->next;
	}
	
	return TRUE;
}

static void for_each_target_folder(GFile * path, FmFileSearchJob * job)
{
	/* FIXME: 	I added error checking.
				I think I freed up the resources as well. */

	GError * error = NULL;
	FmJobErrorAction action;
	GFileEnumerator * enumerator = g_file_enumerate_children(path, gfile_info_query_attribs, G_FILE_QUERY_INFO_NONE, fm_job_get_cancellable(job), &error);

	if(enumerator == NULL)
		action = fm_job_emit_error(FM_JOB(job), error, FM_JOB_ERROR_SEVERE);
	else /* enumerator opened correctly */
	{
		GFileInfo * file_info = g_file_enumerator_next_file(enumerator, fm_job_get_cancellable(job), &error);

		while(file_info != NULL && !fm_job_is_cancelled(FM_JOB(job))) /* g_file_enumerator_next_file returns NULL on error but NULL on finished too */
		{
			//for_each_file_info(file_info, path, job);

			if(run_rules_for_each_file_info(file_info, path, job))
			{
				const char * name = g_file_info_get_name(file_info); /* does not need to be freed */
				GFile * file = g_file_get_child(path, name);
				FmPath * path = fm_path_new_for_gfile(file);
				FmFileInfo * info = fm_file_info_new_from_gfileinfo(path, file_info);
				fm_list_push_tail_noref(job->files, info); /* file info is referenced when created */

				/* TODO: emit file added signal to be handled by the FmFileSearch */

				if(path != NULL)
					fm_path_unref(path);

				if(file != NULL)
					g_object_unref(file);
			}

			if(file_info != NULL)
				g_object_unref(file_info);

			file_info = g_file_enumerator_next_file(enumerator, fm_job_get_cancellable(job), &error);
		}

		if(error != NULL) /* this should catch g_file_enumerator_next_file errors if they pop up */
			action = fm_job_emit_error(FM_JOB(job), error, FM_JOB_ERROR_SEVERE);

		if(!g_file_enumerator_close(enumerator, fm_job_get_cancellable(FM_JOB(job)), &error))
			action = fm_job_emit_error(FM_JOB(job), error, FM_JOB_ERROR_MILD);
	}

	if(enumerator != NULL)
		g_object_unref(enumerator);

	if(error != NULL)
		g_error_free(error);

	if(action == FM_JOB_ABORT)
		fm_job_cancel(FM_JOB(job));
}

static gboolean run_rules_for_each_file_info(GFileInfo * info, GFile * parent, FmFileSearchJob * job)
{
	gboolean ret = FALSE;

	if(!g_file_info_get_is_hidden(info) || job->settings->show_hidden)
	{
		const char * name = g_file_info_get_name(info); /* does not need to be freed */
		GFile * file = g_file_get_child(parent, name);

		FmFileSearchFuncData * data = g_slice_new(FmFileSearchFuncData);

		data->current_file = file;
		data->current_file_info = info;
		data->settings = job->settings;
		data->target_regex = job->target_regex;
		data->target_contains_regex = job->target_contains_regex;
		data->job = job;

		GSList * rules = job->rules;

		while(rules != NULL)
		{
			FmFileSearchRule * search_rule = rules->data;

			FmFileSearchFunc search_function = (*search_rule->function);

			ret = search_function(data, search_rule->user_data);

			if(!ret)
				break;

			rules = rules->next;
		}

		/* recurse upon each directory */
		if(job->settings->recursive && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
			for_each_target_folder(file,job);

		g_object_unref(file);
		g_slice_free(FmFileSearchFuncData, data);
	}

	return ret;
}


//static void for_each_file_info(GFileInfo * info, GFile * parent, FmFileSearchJob * job)
//{	
	/* TODO: error checking ? */

//	if(!g_file_info_get_is_hidden(info) || job->settings->show_hidden)
//	{
//		if((!job->settings->check_minimum_size || g_file_info_get_size(info) >= job->settings->minimum_size) && (!job->settings->check_maximum_size || g_file_info_get_size(info) <= job->settings->maximum_size)) /* file size check */
//		{
//			const char * display_name = g_file_info_get_display_name(info); /* does not need to be freed */
//			const char * name = g_file_info_get_name(info); /* does not need to be freed */
//			GFile * file = g_file_get_child(parent, name);

//			if(job->settings->target == NULL || search(display_name, job->settings->target, SEARCH_TYPE_FILE_NAME, job)) /* target search */
//			{
//				FmPath * path = fm_path_new_for_gfile(file);
//				FmFileInfo * file_info = fm_file_info_new_from_gfileinfo(path, info);
//				FmMimeType * file_mime = fm_file_info_get_mime_type(file_info);
//				const char * file_type = fm_mime_type_get_type(file_mime);
//				const char * target_file_type = (job->settings->target_type != NULL ? fm_mime_type_get_type(job->settings->target_type) : NULL);

//				if(job->settings->target_type == NULL || g_strcmp0(file_type, target_file_type) == 0) /* mime type search */
//				{
//					if(job->settings->target_contains != NULL && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR) /* target content search */
//					{
//						if(file_content_search(file, info, job))
//							fm_list_push_tail_noref(job->files, file_info); /* file info is referenced when created */
//						else
//							fm_file_info_unref(file_info);
//					}
//					else
//						fm_list_push_tail_noref(job->files, file_info); /* file info is referenced when created */
//				}
//				else
//					fm_file_info_unref(file_info);
//
//				if(path != NULL)
//					fm_path_unref(path);
//			}


			/* recurse upon each directory */
//			if(job->settings->recursive && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
//				for_each_target_folder(file,job);

//			if(file != NULL)
//				g_object_unref(file);
//		}
//	}	
//}

static void load_target_folders(FmPath * path, gpointer user_data)
{
	FmFileSearchJob * job = FM_FILE_SEARCH_JOB(user_data);
	job->target_folders = g_slist_append(job->target_folders, fm_path_to_gfile(path));
}

FmFileInfoList * fm_file_search_job_get_files(FmFileSearchJob * job)
{
	return job->files;
}

/* functions for content search rule */

static gboolean file_content_search_ginputstream(FmFileSearchFuncData * data)
{
	/* FIXME: 	I added error checking.
				I think I freed up the resources as well. */
	/* TODO: 	Rewrite this to read into a buffer, but check across the break for matches */
	GError * error = NULL;
	FmJobErrorAction action;
	gboolean ret = FALSE;
	GFileInputStream * io = g_file_read(data->current_file, fm_job_get_cancellable(data->job), &error);

	if(io == G_IO_ERROR_FAILED)
		action = fm_job_emit_error(FM_JOB(data->job), error, FM_JOB_ERROR_SEVERE);
	else /* input stream successfully opened */
	{
		/* FIXME: should I limit the size to read? */
		goffset size = g_file_info_get_size(data->current_file_info);
		char buffer[size];
	
		if(g_input_stream_read(G_INPUT_STREAM(io), &buffer, size, fm_job_get_cancellable(data->job), error) == -1)
			action = fm_job_emit_error(FM_JOB(data->job), error, FM_JOB_ERROR_SEVERE);
		else /* file successfully read into buffer */
		{
			if(content_search(buffer, data))
				ret = TRUE;
		}
		if(!g_input_stream_close(io, fm_job_get_cancellable(data->job), &error))
				action = fm_job_emit_error(FM_JOB(data->job), error, FM_JOB_ERROR_MILD);
	}

	if (io != NULL)
		g_object_unref(io);

	if(error != NULL)
		g_error_free(error);

	if(action == FM_JOB_ABORT)
		fm_job_cancel(FM_JOB(data->job));

	return ret;
}

static gboolean file_content_search_mmap(FmFileSearchFuncData * data)
{
	/* FIXME: 	I reimplemented the error checking; I think it is more sane now. 
				I think I freed up the resources as well.*/

	GError * error = NULL;
	FmJobErrorAction action;
	gboolean ret = FALSE;
	size_t size = (size_t)g_file_info_get_size(data->current_file_info);
	char * path = g_file_get_path(data->current_file);
	char * contents;
	int fd;
	
	fd = open(path, O_RDONLY);
	if(fd == -1)
	{
		error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_FAILED, "Could not open file descriptor for %s\n", path);
		action = fm_job_emit_error(FM_JOB(data->job), error, FM_JOB_ERROR_SEVERE);
	}
	else /* the fd was opened correctly */
	{
		contents = mmap(0, size, PROT_READ, MAP_SHARED, fd, 0);
		if(contents == MAP_FAILED)
		{
			error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_FAILED, "Could not mmap %s\n", path);
			action = fm_job_emit_error(FM_JOB(data->job), error, FM_JOB_ERROR_SEVERE);
		}
		else /* the file was maped to memory correctly */
		{
			if(content_search(contents, data))
				ret = TRUE;

			if(munmap(contents, size) == -1)
			{
				error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_FAILED, "Could not munmap %s\n", path);
				action = fm_job_emit_error(FM_JOB(data->job), error, FM_JOB_ERROR_MILD);
			}
		}

		if(close(fd) == -1)
		{
			error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_FAILED, "Could not close file descriptor for %s\n", path);
			action = fm_job_emit_error(FM_JOB(data->job), error, FM_JOB_ERROR_MILD);
		}
	}

	if(error != NULL)
		g_error_free(error);

	if(path != NULL)
		g_free(path);

	if(action == FM_JOB_ABORT)
		fm_job_cancel(FM_JOB(data->job));

	return ret;
}

static gboolean content_search(char * haystack, FmFileSearchFuncData * data)
{
	gboolean ret = FALSE;

	if(data->settings->content_mode == FM_FILE_SEARCH_MODE_REGEX)
	{
		ret = g_regex_match(data->target_contains_regex, haystack, 0, NULL);
	}
	else
	{
		if(data->settings->case_sensitive)
		{
			if(fnmatch(data->settings->target_contains, haystack, 0) == 0)
				ret = TRUE;
		}
		else
		{
			if(fnmatch(data->settings->target_contains, haystack, FNM_CASEFOLD) == 0)
				ret = TRUE;
		}
	}

	return ret;
}

/* end of functions for content search rule */

/* rule functions */

gboolean fm_file_search_target_rule(FmFileSearchFuncData * data, gpointer user_data)
{
	gboolean ret = FALSE;
	const char * display_name = g_file_info_get_display_name(data->current_file_info); /* does not need to be freed */

	if(data->settings->target_mode == FM_FILE_SEARCH_MODE_REGEX)
	{
		ret = g_regex_match(data->target_regex, display_name, 0, NULL);
	}
	else
	{
		if(data->settings->case_sensitive)
		{
			if(fnmatch(data->settings->target, display_name, 0) == 0)
				ret = TRUE;
		}
		else
		{
			if(fnmatch(data->settings->target, display_name, FNM_CASEFOLD) == 0)
				ret = TRUE;
		}
	}

	return ret;
}

gboolean fm_file_search_target_contains_rule(FmFileSearchFuncData * data, gpointer user_data)
{
	gboolean ret = FALSE;

	if(g_file_info_get_file_type(data->current_file_info) == G_FILE_TYPE_REGULAR && g_file_info_get_size(data->current_file_info) > 0)
	{
		if(g_file_is_native(data->current_file))
			ret = file_content_search_mmap(data);
		else
			ret = file_content_search_ginputstream(data);
	}

	return ret;
}

gboolean fm_file_search_target_type_rule(FmFileSearchFuncData * data, gpointer user_data)
{
	gboolean ret = FALSE;

	FmPath * path = fm_path_new_for_gfile(data->current_file);
	FmFileInfo * file_info = fm_file_info_new_from_gfileinfo(path, data->current_file_info);
	FmMimeType * file_mime = fm_file_info_get_mime_type(file_info);
	const char * file_type = fm_mime_type_get_type(file_mime);
	const char * target_file_type = (data->settings->target_type != NULL ? fm_mime_type_get_type(data->settings->target_type) : NULL);

	if(data->settings->target_type == NULL || g_strcmp0(file_type, target_file_type) == 0)
		ret = TRUE;

	if(file_mime != NULL)
		fm_mime_type_unref(file_mime);

	if(file_info != NULL)
		fm_file_info_unref(file_info);

	if(path != NULL)
		fm_path_unref(path);

	return ret;
}

gboolean fm_file_search_minimum_size_rule(FmFileSearchFuncData * data, gpointer user_data)
{
	gboolean ret = FALSE;

	if((!data->settings->check_minimum_size || g_file_info_get_size(data->current_file_info) >= data->settings->minimum_size))
		ret = TRUE;

	return ret;
}

gboolean fm_file_search_maximum_size_rule(FmFileSearchFuncData * data, gpointer user_data)
{
	gboolean ret = FALSE;

	if((!data->settings->check_maximum_size || g_file_info_get_size(data->current_file_info) <= data->settings->maximum_size))
		ret = TRUE;

	return ret;
}

/* end of rule functions */
