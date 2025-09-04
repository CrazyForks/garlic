#include <errno.h>
//#include "apk/apk.h"
#include "parser/dex/metadata.h"
#include "dalvik/dex_decompile.h"
#include "dalvik/dex_structure.h"
#include "dalvik/dex_class.h"
#include "decompiler/expression_writter.h"
#include "common/output_tools.h"
#include "dex_smali.h"

void apk_status(jd_apk *apk)
{
    pthread_mutex_lock(apk->threadpool->lock);
    apk->done++;
    fflush(stdout);
    backspace(30);
    printf("Progress : %d (%d)", apk->done, apk->added);
    fflush(stdout);
    pthread_mutex_unlock(apk->threadpool->lock);
}

void apk_entry_thread_task(jd_meta_dex *meta)
{
    thread_local_data *tls = get_thread_local_data();
    tls->pool = mem_create_pool();

    dex_analyse_in_apk_task(meta);

    mem_pool_free(tls->pool);
}

void apk_decompile_thread_task(jd_dex_task *task)
{
    thread_local_data *tls = get_thread_local_data();
    tls->pool = mem_create_pool();

    jd_dex *dex = task->dex;
    jd_apk *apk = task->apk;
    dex_class_def *cf = task->cf;

    jsource_file *jf = dex_class_inside(dex, cf, NULL);
    if (jf->parent == NULL) {
        writter_for_class(jf, NULL);
        fclose(jf->source);
    }

    mem_pool_free(tls->pool);

    apk_status(apk);
}

void apk_smali_thread_task(jd_dex_task *task)
{
    thread_local_data *tls = get_thread_local_data();
    tls->pool = mem_create_pool();

    jd_dex *dex = task->dex;
    jd_apk *apk = task->apk;
    dex_class_def *cf = task->cf;

    FILE *stream = dex_class_smali_save_dir(dex, cf);

    dex_class_def_to_smali(dex->meta, cf, stream);

    if (stream != NULL)
        fclose(stream);

    mem_pool_free(tls->pool);

    apk_status(apk);
}

static void apk_decompile_task_start(jd_apk *apk)
{
    struct zip_t *zip = zip_open(apk->path, 0, 'r');
    apk->zip = zip;
    apk->entries_size = zip_entries_total(zip);

    for (int i = 0; i < apk->entries_size; ++i) {
        zip_entry_openbyindex(zip, i);
        string path_in_zip = (string)zip_entry_name(zip);
        if (!str_end_with(path_in_zip, ".dex")) {
            zip_entry_close(zip);
            continue;
        }

        char *buf = NULL;
        size_t buf_size;
        buf_size = zip_entry_size(zip);
        buf = x_alloc_in(apk->pool, buf_size * sizeof(unsigned char));
        zip_entry_noallocread(zip, (void *)buf, buf_size);
        zip_entry_close(zip);

        jd_meta_dex *meta = parse_dex_from_buffer(buf, buf_size);
        jd_dex *dex = dex_init_without_thread(meta);
        meta->source_dir = apk->save_dir;

        for (int j = 0; j < meta->header->class_defs_size; ++j) {
            dex_class_def *cf = &meta->class_defs[j];
            if (apk->type == JD_DEX_TASK_DECOMPILE) {
                if (dex_class_is_inner_class(dex->meta, cf) ||
                    dex_class_is_anonymous_class(dex->meta, cf))
                    continue;
            }

            jd_dex_task *t = make_obj(jd_dex_task);
            t->dex = dex;
            t->cf = cf;
            t->apk = apk;
            t->type = apk->type;
            if (t->type == JD_DEX_TASK_SMALI) {
                threadpool_add(apk->threadpool,
                               &apk_smali_thread_task,
                               t,
                               0);
            }
            else {
                threadpool_add(apk->threadpool,
                               &apk_decompile_thread_task,
                               t,
                               0);
            }
            apk->added++;
        }
    }
    zip_close(zip);
}

static void apk_release(jd_apk *apk)
{
    if (apk->threadpool)
        threadpool_destroy(apk->threadpool, 1);

    mem_pool_free(apk->pool);
    mem_free_pool();
}

void apk_decompile_analyse(string path,
                           string save_dir,
                           int thread_num,
                           jd_dex_task_type type)
{
    mem_init_pool();

    mem_pool *pool = mem_create_pool();
    jd_apk *apk = make_obj_in(jd_apk, pool);
    apk->pool = pool;
    apk->path = path;
    apk->save_dir = save_dir;
    apk->thread_num = thread_num;
    apk->type = type;

    if (thread_num > 1) {
        apk->threadpool = threadpool_create_in(apk->pool, thread_num, 0);
    } else {
        apk->threadpool = NULL;
    }

    apk_decompile_task_start(apk);

    apk_release(apk);
}