(import (chibi test))

(define-syntax ve-eval
  (syntax-rules ()
    ((ve-eval query)
     (boss-eval (EvaluateInEngines
       (List "build/libViewEngine.so"
             "build/libViewEngine.so")
       query)))))


(test-group "DefineView"

  (test "DefineView returns true on success"
        #t
        (ve-eval (DefineView MY_VIEW (Filter (Table (A 1 2 3)) (Greater A 1)))))

  (test "DefineView overwrites existing view"
        #t
        (begin
          (ve-eval (DefineView OVERWRITE (Table (A 1 2))))
          (ve-eval (DefineView OVERWRITE (Table (A 10 20)))))))


(test-group "DefineView failures"

  (test "DefineView with no arguments returns false"
        #f
        (ve-eval (DefineView)))

  (test "DefineView with only name and no body returns false"
        #f
        (ve-eval (DefineView MY_VIEW_NO_BODY)))

  (test "DefineView with too many arguments returns false"
        #f
        (ve-eval (DefineView TOO_MANY (Table (A 1)) (Table (B 2)))))

  (test "DefineView with integer as name returns false"
        #f
        (ve-eval (DefineView 123 (Table (A 1 2 3)))))

  (test "DefineView with string as name returns false"
        #f
        (ve-eval (DefineView "my_view" (Table (A 1 2 3)))))

  (test "DefineView with expression as name returns false"
        #f
        (ve-eval (DefineView (Table (A 1)) (Table (A 1 2 3)))))

  (test "DefineView with ClearViews in body returns false"
        #f
        (ve-eval (DefineView BAD_CLEAR (Filter (ClearViews) (Greater A 1)))))

  (test "DefineView with DropView then QueryView of same view returns false"
        #f
        (ve-eval (DefineView BAD_DROP_QUERY
            (Filter (DropView SOME_VIEW) (QueryView SOME_VIEW)))))

  (test "DefineView with QueryView then DropView of same view returns false"
        #f
        (begin
          (ve-eval (DefineView QTD_TARGET (Table (A 1 2 3))))
          (ve-eval (DefineView BAD_QUERY_DROP
              (Filter (QueryView QTD_TARGET) (DropView QTD_TARGET))))))

  (test "DefineView with QueryView with no argument returns false"
        #f
        (ve-eval (DefineView BAD_QV_NO_ARG (Filter (QueryView) (Greater A 1)))))

  (test "DefineView with QueryView with non-symbol argument returns false"
        #f
        (ve-eval (DefineView BAD_QV_INT_ARG (Filter (QueryView 42) (Greater A 1))))))


(test-group "QueryView"

  (test "QueryView resolves to stored expression"
        '(Filter (Table (A 1 2 3)) (Greater A 1))
        (begin
          (ve-eval (DefineView SIMPLE (Filter (Table (A 1 2 3)) (Greater A 1))))
          (ve-eval (QueryView SIMPLE))))

  (test "QueryView returns latest definition after overwrite"
        '(Table (A 10 20))
        (begin
          (ve-eval (DefineView OVERWRITE2 (Table (A 1 2))))
          (ve-eval (DefineView OVERWRITE2 (Table (A 10 20))))
          (ve-eval (QueryView OVERWRITE2))))

  (test "QueryView same view twice returns same expression"
        '(Table (A 1 2 3))
        (begin
          (ve-eval (DefineView REPEATED (Table (A 1 2 3))))
          (ve-eval (QueryView REPEATED))
          (ve-eval (QueryView REPEATED)))))


(test-group "QueryView errors"

  (test "QueryView with no arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView requires 1 to 3 arguments")
        (ve-eval (QueryView)))

  (test "QueryView with too many arguments throws"
      '(ErrorWhenEvaluatingExpression (||) "QueryView requires 1 to 3 arguments")
      (begin
        (ve-eval (DefineView V1 (Table (A 1))))
        (ve-eval (QueryView V1 #t Structural ExtraArg))))

  (test "QueryView with non-boolean second argument throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView second argument must be a boolean")
        (begin
          (ve-eval (DefineView V1 (Table (A 1))))
          (ve-eval (DefineView V2 (Table (A 2))))
          (ve-eval (QueryView V1 V2))))

  (test "QueryView with non-symbol third argument throws"
      '(ErrorWhenEvaluatingExpression (||) "QueryView third argument must be a symbol")
      (begin
        (ve-eval (DefineView V1 (Table (A 1))))
        (ve-eval (QueryView V1 #t 42))))

  (test "QueryView with unknown integrity mode throws"
      '(ErrorWhenEvaluatingExpression (||) "QueryView unknown integrity mode: UnknownMode")
      (begin
        (ve-eval (DefineView V1 (Table (A 1))))
        (ve-eval (QueryView V1 #t UnknownMode))))

  (test "QueryView on unknown view throws"
        '(ErrorWhenEvaluatingExpression (||) "View not found: NONEXISTENT")
        (ve-eval (QueryView NONEXISTENT)))

  (test "QueryView with integer argument throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView first argument must be a symbol")
        (ve-eval (QueryView 123)))

  (test "QueryView with expression argument throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView first argument must be a symbol")
        (ve-eval (QueryView (Table (A 1 2 3)))))

  (test "QueryView with cache flag true resolves view"
        '(Table (A 1 2 3))
        (begin
          (ve-eval (DefineView CACHED_VIEW (Table (A 1 2 3))))
          (ve-eval (QueryView CACHED_VIEW #t))))

  (test "QueryView with cache flag false resolves view"
        '(Table (A 1 2 3))
        (begin
          (ve-eval (DefineView UNCACHED_VIEW (Table (A 1 2 3))))
          (ve-eval (QueryView UNCACHED_VIEW #f)))))
      

(test-group "Nested views"

  (test "QueryView inside stored expression gets resolved"
        '(Filter (Table (A 1 2 3)) (Greater A 1))
        (begin
          (ve-eval (DefineView INNER (Table (A 1 2 3))))
          (ve-eval (DefineView OUTER (Filter (QueryView INNER) (Greater A 1))))
          (ve-eval (QueryView OUTER))))

  (test "Three levels deep"
        '(GroupBy (Filter (Table (A 1 2 3)) (Greater A 1)) (Sum A))
        (begin
          (ve-eval (DefineView LEVEL1 (Table (A 1 2 3))))
          (ve-eval (DefineView LEVEL2 (Filter (QueryView LEVEL1) (Greater A 1))))
          (ve-eval (DefineView LEVEL3 (GroupBy (QueryView LEVEL2) (Sum A))))
          (ve-eval (QueryView LEVEL3)))))


(test-group "Nested DefineView"

  ;; INNER2 is stored unevaluated inside OUTER3's body — querying INNER2 before
  ;; OUTER3 throws because INNER2 has never been registered
  (test "Querying inner before outer throws"
        '(ErrorWhenEvaluatingExpression (||) "View not found: INNER2")
        (begin
          (ve-eval (DefineView OUTER3
              (Filter (DefineView INNER2 (Table (A 1 2 3))) (Greater A 1))))
          (ve-eval (QueryView INNER2))))

  (test "DefineView with nested DefineView side effect returns false"
      #f
      (ve-eval (DefineView OUTER4
          (Filter (DefineView INNER3 (Table (A 1 2 3))) (Greater A 1)))))

  ;; OUTER5 contains a nested DefineView whose body references OUTER5 itself via QueryView.
  ;; collectDeps finds QueryView OUTER5 inside the nested DefineView body and blocks at define time.
  (test "Nested DefineView referencing outer view blocked at define time"
        #f
        (ve-eval (DefineView OUTER5
            (Filter (DefineView INNER5 (QueryView OUTER5)) (Greater A 1))))))


(test-group "QueryView inside expressions"

  (test "QueryView as argument to Filter passes through resolved"
        '(Filter (Table (A 1 2 3)) (Greater A 1))
        (begin
          (ve-eval (DefineView BASE (Table (A 1 2 3))))
          (ve-eval (Filter (QueryView BASE) (Greater A 1)))))

  (test "QueryView as argument to GroupBy passes through resolved"
        '(GroupBy (Table (A 1 2 3)) (Sum A))
        (begin
          (ve-eval (DefineView BASE2 (Table (A 1 2 3))))
          (ve-eval (GroupBy (QueryView BASE2) (Sum A)))))

  (test "QueryView as argument to Project passes through resolved"
        '(Project (Table (A 1 2 3) (B 4 5 6)) A)
        (begin
          (ve-eval (DefineView BASE3 (Table (A 1 2 3) (B 4 5 6))))
          (ve-eval (Project (QueryView BASE3) A)))))


(test-group "Circular view detection"

  (test "Self-reference blocked at define time"
        #f
        (ve-eval (DefineView SELF (QueryView SELF))))

  (test "Direct cycle blocked - second define returns false"
        #f
        (begin
          (ve-eval (DefineView CYCA (QueryView CYCB)))
          (ve-eval (DefineView CYCB (QueryView CYCA)))))

  (test "Three-step cycle blocked - third define returns false"
        #f
        (begin
          (ve-eval (DefineView TRIIA (QueryView TRIIB)))
          (ve-eval (DefineView TRIIB (QueryView TRIIC)))
          (ve-eval (DefineView TRIIC (QueryView TRIIA)))))

  (test "Blocked define leaves registry unchanged"
        '(ErrorWhenEvaluatingExpression (||) "View not found: CYCB2")
        (begin
          (ve-eval (DefineView CYCA2 (QueryView CYCB2)))
          (ve-eval (DefineView CYCB2 (QueryView CYCA2))) ;; blocked
          (ve-eval (QueryView CYCB2))))                  ;; CYCB2 was never stored

  (test "Registry still usable after blocked cycle define"
        '(Table (A 1 2 3))
        (begin
          (ve-eval (DefineView CLEAN_A (QueryView CLEAN_B)))
          (ve-eval (DefineView CLEAN_B (QueryView CLEAN_A))) ;; blocked
          (ve-eval (DefineView CLEAN_A (Table (A 1 2 3))))   ;; redefine cleanly
          (ve-eval (QueryView CLEAN_A)))))


(test-group "DropView"

  (test "DropView returns true on success"
        #t
        (begin
          (ve-eval (DefineView DROP1 (Table (A 1 2 3))))
          (ve-eval (DropView DROP1))))

  (test "DropView on non-existent view fails gracefully"
        #f
        (ve-eval (DropView NONEXISTENT_DROP)))

  (test "DropView removes view from registry"
        '(ErrorWhenEvaluatingExpression (||) "View not found: DROP2")
        (begin
          (ve-eval (DefineView DROP2 (Table (A 1 2 3))))
          (ve-eval (DropView DROP2))
          (ve-eval (QueryView DROP2))))

  (test "DropView with no arguments fails"
        #f
        (ve-eval (DropView)))

  (test "DropView with too many arguments fails"
        #f
        (begin
          (ve-eval (DefineView DROP3 (Table (A 1 2 3))))
          (ve-eval (DefineView DROP4 (Table (A 1 2 3))))
          (ve-eval (DropView DROP3 DROP4))))

  (test "DropView with integer argument fails"
        #f
        (ve-eval (DropView 123)))

  (test "DropView with expression argument fails"
        #f
        (ve-eval (DropView (Table (A 1 2 3)))))

  (test "DropView on view currently being evaluated blocked at define time"
      #f
      (ve-eval (DefineView DROP5 (Filter (DropView DROP5) (Greater A 1)))))

  (test "DropView fails gracefully after previously failing mid-evaluation"
        #f
        (begin
          (ve-eval (DefineView DROP6 (Filter (DropView DROP6) (Greater A 1))))
          (ve-eval (QueryView DROP6)) ;; throws, but stack must be cleaned up
          (ve-eval (DropView DROP6))))

  (test "DropView blocked if another view depends on it"
        '(ErrorWhenEvaluatingExpression (||) "Cannot drop view DEP_BASE: DEP_CHILD depends on it")
        (begin
          (ve-eval (DefineView DEP_BASE (Table (A 1 2 3))))
          (ve-eval (DefineView DEP_CHILD (QueryView DEP_BASE)))
          (ve-eval (DropView DEP_BASE))))

  (test "DropView succeeds after dependent is dropped first"
        #t
        (begin
          (ve-eval (DefineView DEP_BASE2 (Table (A 1 2 3))))
          (ve-eval (DefineView DEP_CHILD2 (QueryView DEP_BASE2)))
          (ve-eval (DropView DEP_CHILD2))
          (ve-eval (DropView DEP_BASE2))))

  (test "DropView unblocked after dependent is redefined without dependency"
        #t
        (begin
          (ve-eval (DefineView DEP_BASE3 (Table (A 1 2 3))))
          (ve-eval (DefineView DEP_CHILD3 (QueryView DEP_BASE3)))
          (ve-eval (DefineView DEP_CHILD3 (Table (B 4 5 6)))) ;; redefine removes dep
          (ve-eval (DropView DEP_BASE3))))

  (test "DefineView with nested DropView side effect returns false"
      #f
      (begin
        (ve-eval (DefineView DROP_SIDE_DATA (Table (A 1 2 3))))
        (ve-eval (DefineView DROP_SIDE_UNRELATED (Table (B 4 5 6))))
        (ve-eval (DefineView DROP_SIDE_VIEW
            (Filter (QueryView DROP_SIDE_DATA) (DropView DROP_SIDE_UNRELATED))))))

  (test "Unrelated view still exists after blocked side effect define"
      '(Table (B 4 5 6))
      (ve-eval (QueryView DROP_SIDE_UNRELATED))))


(test-group "ClearViews"

  (test "ClearViews returns true"
        #t
        (ve-eval (ClearViews)))

  (test "ClearViews with arguments fails"
        #f
        (ve-eval (ClearViews EXTRA)))

  (test "ClearViews removes all views"
        '(ErrorWhenEvaluatingExpression (||) "View not found: CLEAR1")
        (begin
          (ve-eval (DefineView CLEAR1 (Table (A 1 2 3))))
          (ve-eval (DefineView CLEAR2 (Table (B 4 5 6))))
          (ve-eval (ClearViews))
          (ve-eval (QueryView CLEAR1))))

  (test "ClearViews inside stored expression blocked at define time"
      #f
      (ve-eval (DefineView CLEAR3 (Filter (ClearViews) (Greater A 1)))))

  (test "ClearViews is idempotent on empty registry"
        #t
        (begin
          (ve-eval (ClearViews))
          (ve-eval (ClearViews)))))


(test-group "ListViews"

  (test "ListViews with arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "ListViews does not take any arguments")
        (ve-eval (ListViews EXTRA)))

  (test "ListViews on empty registry returns empty ViewList"
        '(ViewList (Name) (Definition))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (ListViews))))

  (test "ListViews returns single view"
        '(ViewList (Name LIST1) (Definition (Table (A 1 2 3))))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView LIST1 (Table (A 1 2 3))))
          (ve-eval (ListViews))))

  (test "ListViews returns multiple views"
        '(ViewList (Name LIST2 LIST3) (Definition (Table (A 1 2 3)) (Table (B 4 5 6))))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView LIST2 (Table (A 1 2 3))))
          (ve-eval (DefineView LIST3 (Table (B 4 5 6))))
          (ve-eval (ListViews))))

  (test "ListViews reflects overwritten view"
        '(ViewList (Name LIST4) (Definition (Table (A 99))))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView LIST4 (Table (A 1 2 3))))
          (ve-eval (DefineView LIST4 (Table (A 99))))
          (ve-eval (ListViews))))

  (test "ListViews does not show dropped view"
        '(ViewList (Name LIST6) (Definition (Table (B 4 5 6))))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView LIST5 (Table (A 1 2 3))))
          (ve-eval (DefineView LIST6 (Table (B 4 5 6))))
          (ve-eval (DropView LIST5))
          (ve-eval (ListViews))))

  (test "ListViews returns unevaluated expressions"
        '(ViewList (Name LIST7) (Definition (QueryView SOMEOTHER)))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView LIST7 (QueryView SOMEOTHER)))
          (ve-eval (ListViews)))))


(test-group "RegisterTable"

  (test "RegisterTable returns true (eager)"
        #t
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable rt_orders "/data/orders.tbl" "/lib/loader.so" #f rt_orderkey rt_custkey))))

  (test "RegisterTable returns true (lazy)"
        #t
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable rt_lineitem "/data/lineitem.tbl" "/lib/loader.so" #t rt_l_orderkey rt_l_partkey))))

  (test "RegisterTable with zero columns returns true"
        #t
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable rt_nocols "/data/nocols.tbl" "/lib/loader.so" #f))))

  (test "RegisterTable duplicate name returns false"
        #f
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable rt_dup "/data/dup.tbl" "/lib/loader.so" #f rt_dup_col))
          (ve-eval (RegisterTable rt_dup "/data/dup2.tbl" "/lib/loader.so" #f rt_dup_col2))))

  (test "RegisterTable with fewer than 4 arguments returns false"
        #f
        (ve-eval (RegisterTable rt_short "/data/x.tbl" "/lib/loader.so")))

  (test "RegisterTable with no arguments returns false"
        #f
        (ve-eval (RegisterTable)))

  (test "RegisterTable with integer name returns false"
        #f
        (ve-eval (RegisterTable 123 "/data/x.tbl" "/lib/loader.so" #f)))

  (test "RegisterTable with integer url returns false"
        #f
        (ve-eval (RegisterTable rt_int_url 42 "/lib/loader.so" #f)))

  (test "RegisterTable with integer loaderPath returns false"
        #f
        (ve-eval (RegisterTable rt_int_loader "/data/x.tbl" 99 #f)))

  (test "RegisterTable with integer lazy returns false"
        #f
        (ve-eval (RegisterTable rt_int_lazy "/data/x.tbl" "/lib/loader.so" 1)))

  (test "RegisterTable with integer column returns false"
        #f
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable rt_int_col "/data/x.tbl" "/lib/loader.so" #f 123))))

  (test "RegisterTable column name conflicts with existing table name returns false"
        #f
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable rt_base "/data/base.tbl" "/lib/loader.so" #f rt_base_col))
          (ve-eval (RegisterTable rt_other "/data/other.tbl" "/lib/loader.so" #f rt_base))))

  (test "RegisterTable name conflicts with existing column name returns false"
        #f
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable rt_col_owner "/data/owner.tbl" "/lib/loader.so" #f rt_shared_col))
          (ve-eval (RegisterTable rt_shared_col "/data/shared.tbl" "/lib/loader.so" #f rt_other_col))))

  (test "RegisterTable not at top level returns false"
        #f
        (begin
          (ve-eval (ClearTables))
          (ve-eval (DefineView RT_NESTED_VIEW (RegisterTable rt_nested "/data/x.tbl" "/lib/loader.so" #f rt_nested_col)))
          (ve-eval (QueryView RT_NESTED_VIEW)))))


(test-group "DropTable"

  (test "DropTable returns true on success"
        #t
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable dt_tbl "/data/dt.tbl" "/lib/loader.so" #f dt_col))
          (ve-eval (DropTable dt_tbl))))

  (test "DropTable on non-existent table returns false"
        #f
        (begin
          (ve-eval (ClearTables))
          (ve-eval (DropTable dt_nonexistent))))

  (test "DropTable with no arguments returns false"
        #f
        (ve-eval (DropTable)))

  (test "DropTable with too many arguments returns false"
        #f
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable dt_a "/data/a.tbl" "/lib/loader.so" #f dt_col_a))
          (ve-eval (RegisterTable dt_b "/data/b.tbl" "/lib/loader.so" #f dt_col_b))
          (ve-eval (DropTable dt_a dt_b))))

  (test "DropTable with integer argument returns false"
        #f
        (ve-eval (DropTable 123)))

  (test "DropTable removes table from registry"
        '(TableList (Name) (URL) (LoaderPath) (Columns))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable dt_rem "/data/rem.tbl" "/lib/loader.so" #f dt_rem_col))
          (ve-eval (DropTable dt_rem))
          (ve-eval (ListTables))))

  (test "DropTable frees column name for reuse by another table"
        #t
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable dt_free_owner "/data/x.tbl" "/lib/loader.so" #f dt_shared_col))
          (ve-eval (DropTable dt_free_owner))
          (ve-eval (RegisterTable dt_free_reuser "/data/y.tbl" "/lib/loader.so" #f dt_shared_col))))

  (test "DropTable frees table name for reregistration"
        #t
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable dt_reuse "/data/x.tbl" "/lib/loader.so" #f dt_reuse_col))
          (ve-eval (DropTable dt_reuse))
          (ve-eval (RegisterTable dt_reuse "/data/y.tbl" "/lib/loader.so" #f dt_reuse_col))))

  (test "DropTable not at top level returns false"
        #f
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable dt_toplevel "/data/x.tbl" "/lib/loader.so" #f dt_toplevel_col))
          (ve-eval (DefineView DT_NESTED_VIEW (DropTable dt_toplevel)))
          (ve-eval (QueryView DT_NESTED_VIEW)))))


(test-group "ClearTables"

  (test "ClearTables returns true"
        #t
        (ve-eval (ClearTables)))

  (test "ClearTables with arguments returns false"
        #f
        (ve-eval (ClearTables ct_extra)))

  (test "ClearTables removes all tables"
        '(TableList (Name) (URL) (LoaderPath) (Columns))
        (begin
          (ve-eval (RegisterTable ct_a "/data/a.tbl" "/lib/loader.so" #f ct_col_a))
          (ve-eval (RegisterTable ct_b "/data/b.tbl" "/lib/loader.so" #f ct_col_b))
          (ve-eval (ClearTables))
          (ve-eval (ListTables))))

  (test "ClearTables is idempotent on empty registry"
        #t
        (begin
          (ve-eval (ClearTables))
          (ve-eval (ClearTables))))

  (test "ClearTables not at top level returns false"
        #f
        (begin
          (ve-eval (DefineView CT_NESTED_VIEW (ClearTables)))
          (ve-eval (QueryView CT_NESTED_VIEW)))))


(test-group "ListTables"

  (test "ListTables with arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "ListTables does not take any arguments")
        (ve-eval (ListTables lt_extra)))

  (test "ListTables on empty registry returns empty TableList"
        '(TableList (Name) (URL) (LoaderPath) (Columns))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (ListTables))))

  (test "ListTables returns single table"
        '(TableList (Name lt_orders) (URL "/data/orders.tbl") (LoaderPath "/lib/loader.so") (Columns (List lt_orderkey lt_custkey)))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable lt_orders "/data/orders.tbl" "/lib/loader.so" #t lt_orderkey lt_custkey))
          (ve-eval (ListTables))))

  (test "ListTables returns multiple tables sorted alphabetically"
        '(TableList (Name lt_aaa lt_zzz) (URL "/data/aaa.tbl" "/data/zzz.tbl") (LoaderPath "/lib/loader.so" "/lib/loader.so") (Columns (List lt_col1) (List lt_col2)))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable lt_zzz "/data/zzz.tbl" "/lib/loader.so" #f lt_col2))
          (ve-eval (RegisterTable lt_aaa "/data/aaa.tbl" "/lib/loader.so" #f lt_col1))
          (ve-eval (ListTables))))

  (test "ListTables reflects dropped table"
        '(TableList (Name lt_keep) (URL "/data/keep.tbl") (LoaderPath "/lib/loader.so") (Columns (List lt_keep_col)))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable lt_keep "/data/keep.tbl" "/lib/loader.so" #f lt_keep_col))
          (ve-eval (RegisterTable lt_gone "/data/gone.tbl" "/lib/loader.so" #f lt_gone_col))
          (ve-eval (DropTable lt_gone))
          (ve-eval (ListTables))))

  (test "ListTables shows both lazy and eager tables"
        '(TableList (Name lt_eager lt_lazy) (URL "/data/eager.tbl" "/data/lazy.tbl") (LoaderPath "/lib/loader.so" "/lib/loader.so") (Columns (List lt_eager_col) (List lt_lazy_col)))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable lt_lazy "/data/lazy.tbl" "/lib/loader.so" #t lt_lazy_col))
          (ve-eval (RegisterTable lt_eager "/data/eager.tbl" "/lib/loader.so" #f lt_eager_col))
          (ve-eval (ListTables)))))


(test-group "Symbol substitution"

  (ve-eval (ClearViews))

  (test "Lazy table symbol rewrites to Gather"
        '(Gather "/data/sub_lazy.tbl" "/lib/loader.so" (Table) (List))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable sub_lazy "/data/sub_lazy.tbl" "/lib/loader.so" #t sub_col_a sub_col_b))
          (ve-eval sub_lazy)))

  (test "Eager table symbol passes through unchanged"
        'sub_eager
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable sub_eager "/data/sub_eager.tbl" "/lib/loader.so" #f sub_eager_col))
          (ve-eval sub_eager)))

  (test "Unregistered symbol passes through unchanged"
        'sub_unknown
        (begin
          (ve-eval (ClearTables))
          (ve-eval sub_unknown)))

  (test "Lazy table symbol inside expression gets rewritten"
        '(Filter (Gather "/data/sub_filter.tbl" "/lib/loader.so" (Table) (List)) (Greater sub_filter_col 5))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable sub_filter "/data/sub_filter.tbl" "/lib/loader.so" #t sub_filter_col))
          (ve-eval (Filter sub_filter (Greater sub_filter_col 5)))))

  (test "Eager table symbol inside expression passes through unchanged"
        '(Filter sub_eager2 (Greater sub_eager2_col 5))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable sub_eager2 "/data/sub_eager2.tbl" "/lib/loader.so" #f sub_eager2_col))
          (ve-eval (Filter sub_eager2 (Greater sub_eager2_col 5)))))

  (test "After DropTable lazy symbol no longer rewritten"
        'sub_dropped
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable sub_dropped "/data/sub_dropped.tbl" "/lib/loader.so" #t sub_dropped_col))
          (ve-eval (DropTable sub_dropped))
          (ve-eval sub_dropped)))

  (test "After ClearTables lazy symbol no longer rewritten"
        'sub_cleared
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable sub_cleared "/data/sub_cleared.tbl" "/lib/loader.so" #t sub_cleared_col))
          (ve-eval (ClearTables))
          (ve-eval sub_cleared))))


(test-group "Transparent view symbol replacement"

  (ve-eval (ClearViews))

  (test "Bare view symbol resolves same as QueryView"
        '(Table (A 1 2 3))
        (begin
          (ve-eval (DefineView vs_simple (Table (A 1 2 3))))
          (ve-eval vs_simple)))

  (test "Bare view symbol with non-trivial definition resolves"
        '(Filter (Table (A 1 2 3)) (Greater A 1))
        (begin
          (ve-eval (DefineView vs_filtered (Filter (Table (A 1 2 3)) (Greater A 1))))
          (ve-eval vs_filtered)))

  (test "Bare view symbol as argument to outer expression"
        '(Filter (Table (A 1 2 3)) (Greater A 1))
        (begin
          (ve-eval (DefineView vs_inner (Table (A 1 2 3))))
          (ve-eval (Filter vs_inner (Greater A 1)))))

  (test "View shadows same-named lazy table"
        '(Table (A 99))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable vs_shadow "/data/vs_shadow.tbl" "/lib/loader.so" #t vs_shadow_col))
          (ve-eval (DefineView vs_shadow (Table (A 99))))
          (ve-eval vs_shadow)))

  (test "After DropView bare symbol passes through unchanged"
        'vs_dropped
        (begin
          (ve-eval (DefineView vs_dropped (Table (A 1 2 3))))
          (ve-eval (DropView vs_dropped))
          (ve-eval vs_dropped)))

  (test "After ClearViews bare symbol passes through unchanged"
        'vs_cleared
        (begin
          (ve-eval (DefineView vs_cleared (Table (A 1 2 3))))
          (ve-eval (ClearViews))
          (ve-eval vs_cleared)))

  (test "Redefined view bare symbol reflects new definition"
        '(Table (A 99))
        (begin
          (ve-eval (DefineView vs_redef (Table (A 1 2 3))))
          (ve-eval (DefineView vs_redef (Table (A 99))))
          (ve-eval vs_redef))))


(test-group "Column pruning"

  (ve-eval (ClearViews))
  (ve-eval (ClearTables))

  (test "Filter without Project gathers all columns"
      '(Filter (Gather "/data/cp.tbl" "/lib/loader.so" (Table) (List)) (Greater cp_a 1))
      (begin
        (ve-eval (RegisterTable cp_tbl "/data/cp.tbl" "/lib/loader.so" #t cp_a cp_b cp_c))
        (ve-eval (Filter cp_tbl (Greater cp_a 1)))))

  (test "Project referencing subset of columns only gathers those columns"
        '(Project (Gather "/data/cp2.tbl" "/lib/loader.so" (Table) (List cp2_a cp2_b)) cp2_a cp2_b)
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable cp2_tbl "/data/cp2.tbl" "/lib/loader.so" #t cp2_a cp2_b cp2_c cp2_d))
          (ve-eval (Project cp2_tbl cp2_a cp2_b))))

  (test "Filter and Project combined only gathers referenced columns"
        '(Project (Filter (Gather "/data/cp3.tbl" "/lib/loader.so" (Table) (List cp3_a cp3_b)) (Greater cp3_b 5)) cp3_a)
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable cp3_tbl "/data/cp3.tbl" "/lib/loader.so" #t cp3_a cp3_b cp3_c))
          (ve-eval (Project (Filter cp3_tbl (Greater cp3_b 5)) cp3_a))))

  (test "No projections falls back to all columns"
        '(Gather "/data/cp4.tbl" "/lib/loader.so" (Table) (List))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable cp4_tbl "/data/cp4.tbl" "/lib/loader.so" #t cp4_a cp4_b cp4_c))
          (ve-eval cp4_tbl)))

  (test "Eager table is not affected by column pruning"
        '(Project cp5_tbl cp5_a)
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable cp5_tbl "/data/cp5.tbl" "/lib/loader.so" #f cp5_a cp5_b cp5_c))
          (ve-eval (Project cp5_tbl cp5_a)))))


(test-group "Join canonicalisation - rewriter"

  (ve-eval (ClearViews))
  (ve-eval (ClearTables))

  ;; --- Side-swap canonicalisation for INNER joins ---

  (test "Inner join with swapped sides still matches view"
        '(Join customers orders (Equal customer_id order_customer_id))
        (begin
          (ve-eval (RegisterTable customers "/data/customers.tbl" "/lib/loader.so" #f customer_id customer_name))
          (ve-eval (RegisterTable orders "/data/orders.tbl" "/lib/loader.so" #f order_id order_customer_id))
          (ve-eval (DefineView CUSTOMER_ORDERS_VIEW
              (Join customers orders (Equal customer_id order_customer_id))))
          ;; query issued with sides physically reversed
          (ve-eval (Join orders customers (Equal order_customer_id customer_id)))))

  ;; --- Predicate-pair order independence within a fixed side ---

  (test "Multi-predicate inner join matches regardless of predicate order"
        '(Join shipments containers (Equal shipment_port container_port) (Equal shipment_date container_date))
        (begin
          (ve-eval (RegisterTable shipments "/data/shipments.tbl" "/lib/loader.so" #f shipment_port shipment_date))
          (ve-eval (RegisterTable containers "/data/containers.tbl" "/lib/loader.so" #f container_port container_date))
          (ve-eval (DefineView SHIPMENT_CONTAINER_VIEW
              (Join shipments containers (Equal shipment_port container_port) (Equal shipment_date container_date))))
          ;; query lists the two equi-predicates in the opposite order
          (ve-eval (Join shipments containers (Equal shipment_date container_date) (Equal shipment_port container_port)))))

  ;; --- Combined: both swapped sides AND swapped predicate order at once ---

  (test "Inner join matches with both sides and predicate order swapped"
        '(Join employees departments (Equal employee_dept_x dept_x) (Equal employee_dept_y dept_y))
        (begin
          (ve-eval (RegisterTable employees "/data/employees.tbl" "/lib/loader.so" #f employee_dept_x employee_dept_y))
          (ve-eval (RegisterTable departments "/data/departments.tbl" "/lib/loader.so" #f dept_x dept_y))
          (ve-eval (DefineView EMPLOYEE_DEPT_VIEW
              (Join employees departments (Equal employee_dept_x dept_x) (Equal employee_dept_y dept_y))))
          (ve-eval (Join departments employees (Equal dept_y employee_dept_y) (Equal dept_x employee_dept_x)))))

  ;; --- Regression: LEFT/ANTI joins must NOT be side-swapped ---

  (test "LeftJoin with swapped sides does not match (semantics differ)"
        '(LeftJoin returns products (Equal return_product_id product_id))
        (begin
          (ve-eval (RegisterTable products "/data/products.tbl" "/lib/loader.so" #f product_id product_name))
          (ve-eval (RegisterTable returns "/data/returns.tbl" "/lib/loader.so" #f return_id return_product_id))
          (ve-eval (DefineView PRODUCT_RETURNS_VIEW (LeftJoin products returns (Equal product_id return_product_id))))
          ;; sides reversed relative to the view - must remain unrewritten (passes through as-is)
          (ve-eval (LeftJoin returns products (Equal return_product_id product_id)))))

  ;; --- Sanity: unmatched inner join (different tables) must not spuriously match ---

  (test "Inner join over unrelated tables does not match unrelated view"
        '(Join suppliers warehouses (Equal supplier_id warehouse_supplier_id))
        (begin
          (ve-eval (RegisterTable suppliers "/data/suppliers.tbl" "/lib/loader.so" #f supplier_id))
          (ve-eval (RegisterTable warehouses "/data/warehouses.tbl" "/lib/loader.so" #f warehouse_supplier_id))
          (ve-eval (Join suppliers warehouses (Equal supplier_id warehouse_supplier_id)))))

  ;; --- View-on-view dependency chain, exercises recursive expandSignature/cache path ---

  (test "Inner join matches through a nested view dependency, sides and predicates reversed"
        '(Join (Join customers orders (Equal customer_id order_customer_id))
               payments (Equal order_customer_id payment_customer_id))
        (begin
          (ve-eval (RegisterTable payments "/data/payments.tbl" "/lib/loader.so" #f payment_id payment_customer_id))
          (ve-eval (DefineView CUSTOMER_ORDERS_L1
              (Join customers orders (Equal customer_id order_customer_id))))
          (ve-eval (DefineView CUSTOMER_ORDERS_PAYMENTS_L2
              (Join (QueryView CUSTOMER_ORDERS_L1) payments (Equal order_customer_id payment_customer_id))))
          ;; raw query, no embedded QueryView anywhere — sides reversed relative to the view
          (ve-eval (Join payments (Join customers orders (Equal customer_id order_customer_id))
                         (Equal payment_customer_id order_customer_id)))))

  ;; --- Mixed equi-join + residual (non-equi) predicate ---

  (test "Inner join with mixed equi + residual predicate matches regardless of predicate order"
        '(Join products returns (Equal product_id return_product_id) (Greater product_price 5))
        (begin
          (ve-eval (DefineView PRODUCT_RETURNS_FILTERED_VIEW
              (Join products returns (Equal product_id return_product_id) (Greater product_price 5))))
          ;; query lists the residual predicate before the equi-predicate
          (ve-eval (Join products returns (Greater product_price 5) (Equal product_id return_product_id))))))


(test-group "Partial rewriting - residual Filter/Project"

  (ve-eval (ClearViews))
  (ve-eval (ClearTables))

  (ve-eval (RegisterTable pr_products "/data/pr_products.tbl" "/lib/loader.so" #f
             pr_product_id pr_product_name pr_product_price))
  (ve-eval (RegisterTable pr_returns "/data/pr_returns.tbl" "/lib/loader.so" #f
             pr_return_id pr_return_product_id))

  (test "Predicate-only residual: extra Filter applied on top of resolved view"
        '(Filter (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
                 (Greater pr_product_price 5))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView PR_JOIN_VIEW
              (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))))
          (ve-eval (Filter
              (Join pr_returns pr_products (Equal pr_return_product_id pr_product_id))
              (Greater pr_product_price 5)))))

  (test "Projection-only residual: view over-projects, query narrows via residual Project"
        '(Project
          (Project (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
                    pr_product_id pr_product_name pr_product_price pr_return_id)
          pr_product_id)
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView PR_WIDE_VIEW
              (Project (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
                        pr_product_id pr_product_name pr_product_price pr_return_id)))
          (ve-eval (Project
              (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
              pr_product_id))))

  (test "Combined residual: Filter innermost, Project outermost, wrapping resolved view"
        '(Project
          (Filter (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
                  (Greater pr_product_price 5))
          pr_product_id)
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView PR_PLAIN_VIEW
              (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))))
          (ve-eval (Project
              (Filter
                (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
                (Greater pr_product_price 5))
              pr_product_id))))

  (test "Not rewritable: view missing a projected column leaves query unchanged"
        '(Project (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
                   pr_product_id pr_product_price)
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView PR_NARROW_VIEW
              (Project (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
                        pr_product_id pr_return_id)))
          (ve-eval (Project
              (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
              pr_product_id pr_product_price))))
              
  (test "Not rewritable: view predicate absent from query is not silently applied"
        '(Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView PR_FILTERED_VIEW
              (Filter
                (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
                (Greater pr_product_price 5))))
          (ve-eval (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))))))


(test-group "Domain predicate semantics"

  (ve-eval (ClearViews))
  (ve-eval (ClearTables))

  (ve-eval (RegisterTable dp_products "/data/dp_products.tbl" "/lib/loader.so" #f
             dp_product_id dp_product_name dp_product_price))
  (ve-eval (RegisterTable dp_returns "/data/dp_returns.tbl" "/lib/loader.so" #f
             dp_return_id dp_return_product_id))

  ;; --- View domain strictly weaker than query domain: must match with residual ---

  (test "View with Greater(price,50) matches query needing Greater(price,100), residual added"
        '(Filter (Filter (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                          (Greater dp_product_price 50))
                 (Greater dp_product_price 100))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView DP_WEAK_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (Greater dp_product_price 50))))
          (ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 100)))))

  ;; --- View domain strictly stronger than query domain: must NOT match ---

  (test "View with Greater(price,100) does not cover query needing Greater(price,50)"
        '(Filter (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                 (Greater dp_product_price 50))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView DP_STRONG_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (Greater dp_product_price 100))))
          (ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 50)))))

  ;; --- Exact domain match: no residual should be added ---

  (test "View with exact same domain predicate as query needs no residual"
        '(Filter (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                 (Greater dp_product_price 50))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView DP_EXACT_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (Greater dp_product_price 50))))
          (ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 50)))))

  ;; --- Between covered by an equivalent view built from two comparisons ---

  (test "View with Greater+Less matches query needing a narrower Between"
        '(Filter (Filter (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                          (And (Greater dp_product_price 10) (Less dp_product_price 50)))
                 (Between dp_product_price 20 30))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView DP_RANGE_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (And (Greater dp_product_price 10) (Less dp_product_price 50)))))
          (ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Between dp_product_price 20 30)))))

  ;; --- Unrestricted view column vs query domain: usable but residual still needed ---

  (test "View unrestricted on price still needs residual Filter for query's Greater"
        '(Filter (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                 (Greater dp_product_price 5))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView DP_PLAIN_VIEW
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))))
          (ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 5)))))

  ;; --- Single-column Or: real domain union, should match a subset request ---

  (test "View with Or on same column (union) matches a query needing a subset range"
        '(Filter (Filter (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                          (Or (Less dp_product_price 10) (Greater dp_product_price 5)))
                 (Greater dp_product_price 5))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView DP_OR_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (Or (Less dp_product_price 10) (Greater dp_product_price 5)))))
          (ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 5)))))

  ;; --- Cross-column Or: opaque fallback, only exact same expression should match ---

  (test "View with cross-column Or matches only exact same Or expression in query"
        '(Filter (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                 (Or (Greater dp_product_price 5) (Greater dp_return_id 10)))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView DP_CROSS_OR_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (Or (Greater dp_product_price 5) (Greater dp_return_id 10)))))
          (ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Or (Greater dp_product_price 5) (Greater dp_return_id 10))))))

  (test "View with cross-column Or does not match a differently-shaped query predicate"
        '(Filter (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                 (Greater dp_product_price 5))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView DP_CROSS_OR_VIEW2
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (Or (Greater dp_product_price 5) (Greater dp_return_id 10)))))
          (ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 5)))))

  ;; --- NotEqual: split-range domain ---

  (test "View with matching NotEqual needs no residual"
        '(Filter (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                 (NotEqual dp_product_price 5))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView DP_NOTEQUAL_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (NotEqual dp_product_price 5))))
          (ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (NotEqual dp_product_price 5)))))

  (test "View with NotEqual(5) covers query needing Greater(10) via subset range, residual added"
        '(Filter (Filter (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                          (NotEqual dp_product_price 5))
                 (Greater dp_product_price 10))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView DP_NOTEQUAL_VIEW2
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (NotEqual dp_product_price 5))))
          (ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 10)))))

  ;; --- LEFT JOIN with a pre-filtered right (destructive) side: rewriting must be blocked ---

  (test "View is a LeftJoin with filtered right side - query against it is not rewritten"
        '(LeftJoin dp_products
             (Filter dp_returns (Greater dp_return_id 100))
             (Equal dp_product_id dp_return_product_id))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView DP_LEFTJOIN_VIEW
              (LeftJoin dp_products
                 (Filter dp_returns (Greater dp_return_id 100))
                 (Equal dp_product_id dp_return_product_id))))
          (ve-eval (LeftJoin dp_products
                       (Filter dp_returns (Greater dp_return_id 100))
                       (Equal dp_product_id dp_return_product_id)))))

  ;; --- Regression: exact base-table/join match but unrestricted domain column must still residual ---

  (test "Regression: unrestricted view on filtered column always produces residual, never bare QueryView"
        '(Filter (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                 (Greater dp_product_price 5))
        (begin
          (ve-eval (ClearViews))
          (ve-eval (DefineView DP_REGRESSION_VIEW
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))))
          (ve-eval (Filter
              (Join dp_returns dp_products (Equal dp_return_product_id dp_product_id))
              (Greater dp_product_price 5))))))