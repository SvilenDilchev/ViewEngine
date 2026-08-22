(import (chibi test))

;; Single ViewEngine pass. For tests checking VE's own rewrite/substitution
;; output where nothing needs a second pass to unwrap - bare-symbol/Gather
;; substitution with no findRewriting match involved. No real data is loaded
;; here, so Gather (Wisent's operator, not wired in) and ByName references to
;; never-loaded tables are expected to stay symbolic.
(define-syntax ve-eval
  (syntax-rules ()
    ((ve-eval query)
     (boss-eval (EvaluateInEngines
       (List "build/libViewEngine.so")
       query)))))

;; Two ViewEngine passes, no ACE. When findRewriting matches an existing view,
;; VE1 substitutes in a QueryView-style reference, which always wraps itself in
;; WithCaches/Pending/CacheRef (the same protocol explicit QueryView caching
;; uses). Resolving that wrapper - unpacking WithCaches and substituting the
;; CacheRef back down to a plain expression - is ACE's job alone; VE never
;; resolves its own CacheRefs, on any pass. So with ACE absent, the matched
;; view's own definition stays opaque as `(CacheRef viewName Pending|Borrowed)`
;; in the output - VE2 only adds value here by confirming *which* view got
;; matched and constructing the residual Filter/Project wrapped around that
;; reference. Use this (instead of ve-ace-ve-eval) specifically when the
;; matched view's own definition can't be pushed through real ACE without real
;; backing data (e.g. it was built over RegisterTable'd tables with fake
;; loader paths) - i.e. when checking rewriter/scoring logic (which view
;; matched, what residual got added) rather than the resolved content itself.
;; For anything built purely from literal Table data, prefer ve-ace-ve-eval -
;; real ACE resolves it for free and the assertion can check the true
;; materialised result instead of an opaque reference.
(define-syntax ve-ve-eval
  (syntax-rules ()
    ((ve-ve-eval query)
     (boss-eval (EvaluateInEngines
       (List "build/libViewEngine.so"
             "build/libViewEngine.so")
       query)))))

;; Full 3-engine pipeline (VE1 -> ACE -> VE2), for tests that need real
;; execution/materialisation against actual data.
(define-syntax ve-ace-ve-eval
  (syntax-rules ()
    ((ve-ace-ve-eval query)
     (boss-eval (EvaluateInEngines
       (List "build/libViewEngine.so"
             "build/libArrowComputeEngine.so"
             "build/libViewEngine.so")
       query)))))


(test-group "DefineView"

  (test "DefineView returns true on success"
        #t
        (ve-ace-ve-eval (DefineView MY_VIEW (Filter (Table (A 1 2 3)) (Greater A 1)))))

  (test "DefineView overwrites existing view"
        #t
        (begin
          (ve-ace-ve-eval (DefineView OVERWRITE (Table (A 1 2))))
          (ve-ace-ve-eval (DefineView OVERWRITE (Table (A 10 20)))))))


(test-group "DefineView failures"

  (test "DefineView with no arguments returns false"
        #f
        (ve-ace-ve-eval (DefineView)))

  (test "DefineView with only name and no body returns false"
        #f
        (ve-ace-ve-eval (DefineView MY_VIEW_NO_BODY)))

  (test "DefineView with too many arguments returns false"
        #f
        (ve-ace-ve-eval (DefineView TOO_MANY (Table (A 1)) (Table (B 2)))))

  (test "DefineView with integer as name returns false"
        #f
        (ve-ace-ve-eval (DefineView 123 (Table (A 1 2 3)))))

  (test "DefineView with string as name returns false"
        #f
        (ve-ace-ve-eval (DefineView "my_view" (Table (A 1 2 3)))))

  (test "DefineView with expression as name returns false"
        #f
        (ve-ace-ve-eval (DefineView (Table (A 1)) (Table (A 1 2 3)))))

  (test "DefineView with ClearViews in body returns false"
        #f
        (ve-ace-ve-eval (DefineView BAD_CLEAR (Filter (ClearViews) (Greater A 1)))))

  (test "DefineView with DropView then QueryView of same view returns false"
        #f
        (ve-ace-ve-eval (DefineView BAD_DROP_QUERY
            (Filter (DropView SOME_VIEW) (QueryView SOME_VIEW Defer)))))

  (test "DefineView with QueryView then DropView of same view returns false"
        #f
        (begin
          (ve-ace-ve-eval (DefineView QTD_TARGET (Table (A 1 2 3))))
          (ve-ace-ve-eval (DefineView BAD_QUERY_DROP
              (Filter (QueryView QTD_TARGET Defer) (DropView QTD_TARGET))))))

  (test "DefineView with QueryView with no argument returns false"
        #f
        (ve-ace-ve-eval (DefineView BAD_QV_NO_ARG (Filter (QueryView) (Greater A 1)))))

  (test "DefineView with QueryView with non-symbol argument returns false"
        #f
        (ve-ace-ve-eval (DefineView BAD_QV_INT_ARG (Filter (QueryView 42) (Greater A 1))))))


(test-group "QueryView"

  ;; These are resolved via ve-ace-ve-eval (real ACE), not ve-ve-eval: resolving a
  ;; QueryView/CacheRef is ACE's job, not VE's, on any pass - see the ve-ve-eval
  ;; comment above. All bodies here are literal Table data, so real ACE needs no
  ;; backing data to materialise them, and the expected values are now the real
  ;; computed results rather than the unevaluated stored expression.

  (test "QueryView resolves to stored expression"
        '(Table (A 2 3))
        (begin
          (ve-ace-ve-eval (DefineView SIMPLE (Filter (Table (A 1 2 3)) (Greater A 1))))
          (ve-ace-ve-eval (QueryView SIMPLE Defer))))

  (test "QueryView returns latest definition after overwrite"
        '(Table (A 10 20))
        (begin
          (ve-ace-ve-eval (DefineView OVERWRITE2 (Table (A 1 2))))
          (ve-ace-ve-eval (DefineView OVERWRITE2 (Table (A 10 20))))
          (ve-ace-ve-eval (QueryView OVERWRITE2 Defer))))

  (test "QueryView same view twice returns same expression"
        '(Table (A 1 2 3))
        (begin
          (ve-ace-ve-eval (DefineView REPEATED (Table (A 1 2 3))))
          (ve-ace-ve-eval (QueryView REPEATED Defer))
          (ve-ace-ve-eval (QueryView REPEATED Defer)))))


(test-group "QueryView errors"

  (test "QueryView with no arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView requires 1 to 4 arguments")
        (ve-ace-ve-eval (QueryView)))

  (test "QueryView with too many arguments throws"
      '(ErrorWhenEvaluatingExpression (||) "QueryView requires 1 to 4 arguments")
      (begin
        (ve-ace-ve-eval (DefineView V1 (Table (A 1))))
        (ve-ace-ve-eval (QueryView V1 Defer Standard Structural ExtraArg))))

  (test "QueryView with non-symbol second argument throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView second argument must be a symbol: Admit, Reject, or Defer")
        (begin
          (ve-ace-ve-eval (DefineView V1 (Table (A 1))))
          (ve-ace-ve-eval (QueryView V1 42))))

  (test "QueryView with unknown decision symbol throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView second argument must be a symbol: Admit, Reject, or Defer")
        (begin
          (ve-ace-ve-eval (DefineView V1 (Table (A 1))))
          (ve-ace-ve-eval (DefineView V2 (Table (A 2))))
          (ve-ace-ve-eval (QueryView V1 V2))))

  (test "QueryView with non-symbol third argument throws"
      '(ErrorWhenEvaluatingExpression (||) "QueryView third argument must be a symbol")
      (begin
        (ve-ace-ve-eval (DefineView V1 (Table (A 1))))
        (ve-ace-ve-eval (QueryView V1 Defer 42))))

  (test "QueryView with unknown integrity mode throws"
      '(ErrorWhenEvaluatingExpression (||) "QueryView unknown integrity mode: UnknownMode")
      (begin
        (ve-ace-ve-eval (DefineView V1 (Table (A 1))))
        (ve-ace-ve-eval (QueryView V1 Defer Standard UnknownMode))))

  (test "QueryView on unknown view throws"
        '(ErrorWhenEvaluatingExpression (||) "View not found: NONEXISTENT")
        (ve-ace-ve-eval (QueryView NONEXISTENT)))

  (test "QueryView with integer argument throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView first argument must be a symbol")
        (ve-ace-ve-eval (QueryView 123)))

  (test "QueryView with expression argument throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView first argument must be a symbol")
        (ve-ace-ve-eval (QueryView (Table (A 1 2 3)))))

  (test "QueryView with cache flag true resolves view"
        '(Table (A 1 2 3))
        (begin
          (ve-ace-ve-eval (DefineView CACHED_VIEW (Table (A 1 2 3))))
          (ve-ace-ve-eval (QueryView CACHED_VIEW Admit))))

  (test "QueryView with cache flag false resolves view"
        '(Table (A 1 2 3))
        (begin
          (ve-ace-ve-eval (DefineView UNCACHED_VIEW (Table (A 1 2 3))))
          (ve-ace-ve-eval (QueryView UNCACHED_VIEW Reject)))))
      

(test-group "Nested views"

  ;; ve-ace-ve-eval, not ve-ve-eval - see comment on the "QueryView" group above.

  (test "QueryView inside stored expression gets resolved"
        '(Table (A 2 3))
        (begin
          (ve-ace-ve-eval (DefineView INNER (Table (A 1 2 3))))
          (ve-ace-ve-eval (DefineView OUTER (Filter (QueryView INNER Defer) (Greater A 1))))
          (ve-ace-ve-eval (QueryView OUTER Defer))))

  (test "Three levels deep"
        '(Table (|sum(A)| 5))
        (begin
          (ve-ace-ve-eval (DefineView LEVEL1 (Table (A 1 2 3))))
          (ve-ace-ve-eval (DefineView LEVEL2 (Filter (QueryView LEVEL1 Defer) (Greater A 1))))
          (ve-ace-ve-eval (DefineView LEVEL3 (GroupBy (QueryView LEVEL2 Defer) (Sum A))))
          (ve-ace-ve-eval (QueryView LEVEL3 Defer)))))


(test-group "Nested DefineView"

  ;; INNER2 is stored unevaluated inside OUTER3's body — querying INNER2 before
  ;; OUTER3 throws because INNER2 has never been registered
  (test "Querying inner before outer throws"
        '(ErrorWhenEvaluatingExpression (||) "View not found: INNER2")
        (begin
          (ve-ace-ve-eval (DefineView OUTER3
              (Filter (DefineView INNER2 (Table (A 1 2 3))) (Greater A 1))))
          (ve-ace-ve-eval (QueryView INNER2))))

  (test "DefineView with nested DefineView side effect returns false"
      #f
      (ve-ace-ve-eval (DefineView OUTER4
          (Filter (DefineView INNER3 (Table (A 1 2 3))) (Greater A 1)))))

  ;; OUTER5 contains a nested DefineView whose body references OUTER5 itself via QueryView.
  ;; collectDeps finds QueryView OUTER5 inside the nested DefineView body and blocks at define time.
  (test "Nested DefineView referencing outer view blocked at define time"
        #f
        (ve-ace-ve-eval (DefineView OUTER5
            (Filter (DefineView INNER5 (QueryView OUTER5 Defer)) (Greater A 1))))))


(test-group "QueryView inside expressions"

  ;; ve-ace-ve-eval, not ve-ve-eval - see comment on the "QueryView" group above.

  (test "QueryView as argument to Filter passes through resolved"
        '(Table (A 2 3))
        (begin
          (ve-ace-ve-eval (DefineView BASE (Table (A 1 2 3))))
          (ve-ace-ve-eval (Filter (QueryView BASE Defer) (Greater A 1)))))

  (test "QueryView as argument to GroupBy passes through resolved"
        '(Table (|sum(A)| 6))
        (begin
          (ve-ace-ve-eval (DefineView BASE2 (Table (A 1 2 3))))
          (ve-ace-ve-eval (GroupBy (QueryView BASE2 Defer) (Sum A)))))

  (test "QueryView as argument to Project passes through resolved"
        '(Table (A 1 2 3))
        (begin
          (ve-ace-ve-eval (DefineView BASE3 (Table (A 1 2 3) (B 4 5 6))))
          (ve-ace-ve-eval (Project (QueryView BASE3 Defer) A)))))


(test-group "Circular view detection"

  (test "Self-reference blocked at define time"
        #f
        (ve-ace-ve-eval (DefineView SELF (QueryView SELF Defer))))

  (test "Direct cycle blocked - second define returns false"
        #f
        (begin
          (ve-ace-ve-eval (DefineView CYCA (QueryView CYCB Defer)))
          (ve-ace-ve-eval (DefineView CYCB (QueryView CYCA Defer)))))

  (test "Three-step cycle blocked - third define returns false"
        #f
        (begin
          (ve-ace-ve-eval (DefineView TRIIA (QueryView TRIIB Defer)))
          (ve-ace-ve-eval (DefineView TRIIB (QueryView TRIIC Defer)))
          (ve-ace-ve-eval (DefineView TRIIC (QueryView TRIIA Defer)))))

  (test "Blocked define leaves registry unchanged"
        '(ErrorWhenEvaluatingExpression (||) "View not found: CYCB2")
        (begin
          (ve-ace-ve-eval (DefineView CYCA2 (QueryView CYCB2 Defer)))
          (ve-ace-ve-eval (DefineView CYCB2 (QueryView CYCA2 Defer))) ;; blocked
          (ve-ace-ve-eval (QueryView CYCB2))))                  ;; CYCB2 was never stored

  (test "Registry still usable after blocked cycle define"
        '(Table (A 1 2 3))
        (begin
          (ve-ace-ve-eval (DefineView CLEAN_A (QueryView CLEAN_B Defer)))
          (ve-ace-ve-eval (DefineView CLEAN_B (QueryView CLEAN_A Defer))) ;; blocked
          (ve-ace-ve-eval (DefineView CLEAN_A (Table (A 1 2 3))))   ;; redefine cleanly
          (ve-ace-ve-eval (QueryView CLEAN_A Defer)))))


(test-group "DropView"

  (test "DropView returns true on success"
        #t
        (begin
          (ve-ace-ve-eval (DefineView DROP1 (Table (A 1 2 3))))
          (ve-ace-ve-eval (DropView DROP1))))

  (test "DropView on non-existent view fails gracefully"
        #f
        (ve-ace-ve-eval (DropView NONEXISTENT_DROP)))

  (test "DropView removes view from registry"
        '(ErrorWhenEvaluatingExpression (||) "View not found: DROP2")
        (begin
          (ve-ace-ve-eval (DefineView DROP2 (Table (A 1 2 3))))
          (ve-ace-ve-eval (DropView DROP2))
          (ve-ace-ve-eval (QueryView DROP2))))

  (test "DropView with no arguments fails"
        #f
        (ve-ace-ve-eval (DropView)))

  (test "DropView with too many arguments fails"
        #f
        (begin
          (ve-ace-ve-eval (DefineView DROP3 (Table (A 1 2 3))))
          (ve-ace-ve-eval (DefineView DROP4 (Table (A 1 2 3))))
          (ve-ace-ve-eval (DropView DROP3 DROP4))))

  (test "DropView with integer argument fails"
        #f
        (ve-ace-ve-eval (DropView 123)))

  (test "DropView with expression argument fails"
        #f
        (ve-ace-ve-eval (DropView (Table (A 1 2 3)))))

  (test "DropView on view currently being evaluated blocked at define time"
      #f
      (ve-ace-ve-eval (DefineView DROP5 (Filter (DropView DROP5) (Greater A 1)))))

  (test "DropView fails gracefully after previously failing mid-evaluation"
        #f
        (begin
          (ve-ace-ve-eval (DefineView DROP6 (Filter (DropView DROP6) (Greater A 1))))
          (ve-ace-ve-eval (QueryView DROP6)) ;; throws, but stack must be cleaned up
          (ve-ace-ve-eval (DropView DROP6))))

  (test "DropView blocked if another view depends on it"
        '(ErrorWhenEvaluatingExpression (||) "Cannot drop view DEP_BASE: DEP_CHILD depends on it")
        (begin
          (ve-ace-ve-eval (DefineView DEP_BASE (Table (A 1 2 3))))
          (ve-ace-ve-eval (DefineView DEP_CHILD (QueryView DEP_BASE Defer)))
          (ve-ace-ve-eval (DropView DEP_BASE))))

  (test "DropView succeeds after dependent is dropped first"
        #t
        (begin
          (ve-ace-ve-eval (DefineView DEP_BASE2 (Table (A 1 2 3))))
          (ve-ace-ve-eval (DefineView DEP_CHILD2 (QueryView DEP_BASE2 Defer)))
          (ve-ace-ve-eval (DropView DEP_CHILD2))
          (ve-ace-ve-eval (DropView DEP_BASE2))))

  (test "DropView unblocked after dependent is redefined without dependency"
        #t
        (begin
          (ve-ace-ve-eval (DefineView DEP_BASE3 (Table (A 1 2 3))))
          (ve-ace-ve-eval (DefineView DEP_CHILD3 (QueryView DEP_BASE3 Defer)))
          (ve-ace-ve-eval (DefineView DEP_CHILD3 (Table (B 4 5 6)))) ;; redefine removes dep
          (ve-ace-ve-eval (DropView DEP_BASE3))))

  (test "DefineView with nested DropView side effect returns false"
      #f
      (begin
        (ve-ace-ve-eval (DefineView DROP_SIDE_DATA (Table (A 1 2 3))))
        (ve-ace-ve-eval (DefineView DROP_SIDE_UNRELATED (Table (B 4 5 6))))
        (ve-ace-ve-eval (DefineView DROP_SIDE_VIEW
            (Filter (QueryView DROP_SIDE_DATA Defer) (DropView DROP_SIDE_UNRELATED))))))

  (test "Unrelated view still exists after blocked side effect define"
      '(Table (B 4 5 6))
      (ve-ace-ve-eval (QueryView DROP_SIDE_UNRELATED Defer))))


(test-group "ClearViews"

  (test "ClearViews returns true"
        #t
        (ve-ace-ve-eval (ClearViews)))

  (test "ClearViews with arguments fails"
        #f
        (ve-ace-ve-eval (ClearViews EXTRA)))

  (test "ClearViews removes all views"
        '(ErrorWhenEvaluatingExpression (||) "View not found: CLEAR1")
        (begin
          (ve-ace-ve-eval (DefineView CLEAR1 (Table (A 1 2 3))))
          (ve-ace-ve-eval (DefineView CLEAR2 (Table (B 4 5 6))))
          (ve-ace-ve-eval (ClearViews))
          (ve-ace-ve-eval (QueryView CLEAR1))))

  (test "ClearViews inside stored expression blocked at define time"
      #f
      (ve-ace-ve-eval (DefineView CLEAR3 (Filter (ClearViews) (Greater A 1)))))

  (test "ClearViews is idempotent on empty registry"
        #t
        (begin
          (ve-ace-ve-eval (ClearViews))
          (ve-ace-ve-eval (ClearViews)))))


(test-group "ListViews"

  (test "ListViews with arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "ListViews does not take any arguments")
        (ve-ace-ve-eval (ListViews EXTRA)))

  (test "ListViews on empty registry returns empty ViewList"
        '(ViewList (Name) (Definition))
        (begin
          (ve-ace-ve-eval (ClearViews))
          (ve-ace-ve-eval (ListViews))))

  (test "ListViews returns single view"
        '(ViewList (Name LIST1) (Definition (Table (A 1 2 3))))
        (begin
          (ve-ace-ve-eval (ClearViews))
          (ve-ace-ve-eval (DefineView LIST1 (Table (A 1 2 3))))
          (ve-ace-ve-eval (ListViews))))

  (test "ListViews returns multiple views"
        '(ViewList (Name LIST2 LIST3) (Definition (Table (A 1 2 3)) (Table (B 4 5 6))))
        (begin
          (ve-ace-ve-eval (ClearViews))
          (ve-ace-ve-eval (DefineView LIST2 (Table (A 1 2 3))))
          (ve-ace-ve-eval (DefineView LIST3 (Table (B 4 5 6))))
          (ve-ace-ve-eval (ListViews))))

  (test "ListViews reflects overwritten view"
        '(ViewList (Name LIST4) (Definition (Table (A 99))))
        (begin
          (ve-ace-ve-eval (ClearViews))
          (ve-ace-ve-eval (DefineView LIST4 (Table (A 1 2 3))))
          (ve-ace-ve-eval (DefineView LIST4 (Table (A 99))))
          (ve-ace-ve-eval (ListViews))))

  (test "ListViews does not show dropped view"
        '(ViewList (Name LIST6) (Definition (Table (B 4 5 6))))
        (begin
          (ve-ace-ve-eval (ClearViews))
          (ve-ace-ve-eval (DefineView LIST5 (Table (A 1 2 3))))
          (ve-ace-ve-eval (DefineView LIST6 (Table (B 4 5 6))))
          (ve-ace-ve-eval (DropView LIST5))
          (ve-ace-ve-eval (ListViews))))

  (test "ListViews returns unevaluated expressions"
        '(ViewList (Name LIST7) (Definition (QueryView SOMEOTHER Defer)))
        (begin
          (ve-ace-ve-eval (ClearViews))
          (ve-ace-ve-eval (DefineView LIST7 (QueryView SOMEOTHER Defer)))
          (ve-ace-ve-eval (ListViews)))))


(test-group "RegisterTable"

  (test "RegisterTable returns true (eager)"
        #t
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable rt_orders "/data/orders.tbl" "/lib/loader.so" #f rt_orderkey rt_custkey))))

  (test "RegisterTable returns true (lazy)"
        #t
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable rt_lineitem "/data/lineitem.tbl" "/lib/loader.so" #t rt_l_orderkey rt_l_partkey))))

  (test "RegisterTable with zero columns returns true"
        #t
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable rt_nocols "/data/nocols.tbl" "/lib/loader.so" #f))))

  (test "RegisterTable duplicate name returns false"
        #f
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable rt_dup "/data/dup.tbl" "/lib/loader.so" #f rt_dup_col))
          (ve-ace-ve-eval (RegisterTable rt_dup "/data/dup2.tbl" "/lib/loader.so" #f rt_dup_col2))))

  (test "RegisterTable with fewer than 4 arguments returns false"
        #f
        (ve-ace-ve-eval (RegisterTable rt_short "/data/x.tbl" "/lib/loader.so")))

  (test "RegisterTable with no arguments returns false"
        #f
        (ve-ace-ve-eval (RegisterTable)))

  (test "RegisterTable with integer name returns false"
        #f
        (ve-ace-ve-eval (RegisterTable 123 "/data/x.tbl" "/lib/loader.so" #f)))

  (test "RegisterTable with integer url returns false"
        #f
        (ve-ace-ve-eval (RegisterTable rt_int_url 42 "/lib/loader.so" #f)))

  (test "RegisterTable with integer loaderPath returns false"
        #f
        (ve-ace-ve-eval (RegisterTable rt_int_loader "/data/x.tbl" 99 #f)))

  (test "RegisterTable with integer lazy returns false"
        #f
        (ve-ace-ve-eval (RegisterTable rt_int_lazy "/data/x.tbl" "/lib/loader.so" 1)))

  (test "RegisterTable with integer column returns false"
        #f
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable rt_int_col "/data/x.tbl" "/lib/loader.so" #f 123))))

  (test "RegisterTable column name conflicts with existing table name returns false"
        #f
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable rt_base "/data/base.tbl" "/lib/loader.so" #f rt_base_col))
          (ve-ace-ve-eval (RegisterTable rt_other "/data/other.tbl" "/lib/loader.so" #f rt_base))))

  (test "RegisterTable name conflicts with existing column name returns false"
        #f
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable rt_col_owner "/data/owner.tbl" "/lib/loader.so" #f rt_shared_col))
          (ve-ace-ve-eval (RegisterTable rt_shared_col "/data/shared.tbl" "/lib/loader.so" #f rt_other_col))))

  (test "RegisterTable not at top level returns false"
        '(TestWrapper #f)
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (TestWrapper (RegisterTable rt_nested "/data/x.tbl" "/lib/loader.so" #f rt_nested_col))))))


(test-group "DropTable"

  (test "DropTable returns true on success"
        #t
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable dt_tbl "/data/dt.tbl" "/lib/loader.so" #f dt_col))
          (ve-ace-ve-eval (DropTable dt_tbl))))

  (test "DropTable on non-existent table returns false"
        #f
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (DropTable dt_nonexistent))))

  (test "DropTable with no arguments returns false"
        #f
        (ve-ace-ve-eval (DropTable)))

  (test "DropTable with too many arguments returns false"
        #f
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable dt_a "/data/a.tbl" "/lib/loader.so" #f dt_col_a))
          (ve-ace-ve-eval (RegisterTable dt_b "/data/b.tbl" "/lib/loader.so" #f dt_col_b))
          (ve-ace-ve-eval (DropTable dt_a dt_b))))

  (test "DropTable with integer argument returns false"
        #f
        (ve-ace-ve-eval (DropTable 123)))

  (test "DropTable removes table from registry"
        '(TableList (Name) (URL) (LoaderPath) (Columns))
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable dt_rem "/data/rem.tbl" "/lib/loader.so" #f dt_rem_col))
          (ve-ace-ve-eval (DropTable dt_rem))
          (ve-ace-ve-eval (ListTables))))

  (test "DropTable frees column name for reuse by another table"
        #t
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable dt_free_owner "/data/x.tbl" "/lib/loader.so" #f dt_shared_col))
          (ve-ace-ve-eval (DropTable dt_free_owner))
          (ve-ace-ve-eval (RegisterTable dt_free_reuser "/data/y.tbl" "/lib/loader.so" #f dt_shared_col))))

  (test "DropTable frees table name for reregistration"
        #t
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable dt_reuse "/data/x.tbl" "/lib/loader.so" #f dt_reuse_col))
          (ve-ace-ve-eval (DropTable dt_reuse))
          (ve-ace-ve-eval (RegisterTable dt_reuse "/data/y.tbl" "/lib/loader.so" #f dt_reuse_col))))

  (test "DropTable not at top level returns false"
        '(TestWrapper #f)
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable dt_toplevel "/data/x.tbl" "/lib/loader.so" #f dt_toplevel_col))
          (ve-ace-ve-eval (TestWrapper (DropTable dt_toplevel))))))


(test-group "ClearTables"

  (test "ClearTables returns true"
        #t
        (ve-ace-ve-eval (ClearTables)))

  (test "ClearTables with arguments returns false"
        #f
        (ve-ace-ve-eval (ClearTables ct_extra)))

  (test "ClearTables removes all tables"
        '(TableList (Name) (URL) (LoaderPath) (Columns))
        (begin
          (ve-ace-ve-eval (RegisterTable ct_a "/data/a.tbl" "/lib/loader.so" #f ct_col_a))
          (ve-ace-ve-eval (RegisterTable ct_b "/data/b.tbl" "/lib/loader.so" #f ct_col_b))
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (ListTables))))

  (test "ClearTables is idempotent on empty registry"
        #t
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (ClearTables))))

  (test "ClearTables not at top level returns false"
        '(TestWrapper #f)
        (ve-ace-ve-eval (TestWrapper (ClearTables)))))


(test-group "ListTables"

  (test "ListTables with arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "ListTables does not take any arguments")
        (ve-ace-ve-eval (ListTables lt_extra)))

  (test "ListTables on empty registry returns empty TableList"
        '(TableList (Name) (URL) (LoaderPath) (Columns))
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (ListTables))))

  (test "ListTables returns single table"
        '(TableList (Name lt_orders) (URL "/data/orders.tbl") (LoaderPath "/lib/loader.so") (Columns (List lt_orderkey lt_custkey)))
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable lt_orders "/data/orders.tbl" "/lib/loader.so" #t lt_orderkey lt_custkey))
          (ve-ace-ve-eval (ListTables))))

  (test "ListTables returns multiple tables sorted alphabetically"
        '(TableList (Name lt_aaa lt_zzz) (URL "/data/aaa.tbl" "/data/zzz.tbl") (LoaderPath "/lib/loader.so" "/lib/loader.so") (Columns (List lt_col1) (List lt_col2)))
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable lt_zzz "/data/zzz.tbl" "/lib/loader.so" #f lt_col2))
          (ve-ace-ve-eval (RegisterTable lt_aaa "/data/aaa.tbl" "/lib/loader.so" #f lt_col1))
          (ve-ace-ve-eval (ListTables))))

  (test "ListTables reflects dropped table"
        '(TableList (Name lt_keep) (URL "/data/keep.tbl") (LoaderPath "/lib/loader.so") (Columns (List lt_keep_col)))
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable lt_keep "/data/keep.tbl" "/lib/loader.so" #f lt_keep_col))
          (ve-ace-ve-eval (RegisterTable lt_gone "/data/gone.tbl" "/lib/loader.so" #f lt_gone_col))
          (ve-ace-ve-eval (DropTable lt_gone))
          (ve-ace-ve-eval (ListTables))))

  (test "ListTables shows both lazy and eager tables"
        '(TableList (Name lt_eager lt_lazy) (URL "/data/eager.tbl" "/data/lazy.tbl") (LoaderPath "/lib/loader.so" "/lib/loader.so") (Columns (List lt_eager_col) (List lt_lazy_col)))
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable lt_lazy "/data/lazy.tbl" "/lib/loader.so" #t lt_lazy_col))
          (ve-ace-ve-eval (RegisterTable lt_eager "/data/eager.tbl" "/lib/loader.so" #f lt_eager_col))
          (ve-ace-ve-eval (ListTables)))))


(test-group "Symbol substitution"

  (ve-eval (ClearViews))

  (test "Lazy table symbol rewrites to Gather"
        '(Gather "/data/sub_lazy.tbl" "/lib/loader.so" (Table) (List))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable sub_lazy "/data/sub_lazy.tbl" "/lib/loader.so" #t sub_col_a sub_col_b))
          (ve-eval sub_lazy)))

  ;; Eager tables have no bare-symbol resolution mechanism in ACE (only ByName
  ;; does the lookup against ACE's materialized-name table) - VE wraps the
  ;; symbol in ByName rather than passing it through unchanged.
  (test "Eager table symbol is wrapped in ByName"
        '(ByName sub_eager)
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

  (test "Eager table symbol inside expression is wrapped in ByName"
        '(Filter (ByName sub_eager2) (Greater sub_eager2_col 5))
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

  (ve-ve-eval (ClearViews))

  ;; ve-ace-ve-eval, not ve-ve-eval - see comment on the "QueryView" group above.
  ;; (vs_shadow below registers a lazy table but the same-named view always shadows
  ;; it, so the query never actually reaches the table's fake Gather path.)

  (test "Bare view symbol resolves same as QueryView"
        '(Table (A 1 2 3))
        (begin
          (ve-ace-ve-eval (DefineView vs_simple (Table (A 1 2 3))))
          (ve-ace-ve-eval vs_simple)))

  (test "Bare view symbol with non-trivial definition resolves"
        '(Table (A 2 3))
        (begin
          (ve-ace-ve-eval (DefineView vs_filtered (Filter (Table (A 1 2 3)) (Greater A 1))))
          (ve-ace-ve-eval vs_filtered)))

  (test "Bare view symbol as argument to outer expression"
        '(Table (A 2 3))
        (begin
          (ve-ace-ve-eval (DefineView vs_inner (Table (A 1 2 3))))
          (ve-ace-ve-eval (Filter vs_inner (Greater A 1)))))

  (test "View shadows same-named lazy table"
        '(Table (A 99))
        (begin
          (ve-ace-ve-eval (ClearTables))
          (ve-ace-ve-eval (RegisterTable vs_shadow "/data/vs_shadow.tbl" "/lib/loader.so" #t vs_shadow_col))
          (ve-ace-ve-eval (DefineView vs_shadow (Table (A 99))))
          (ve-ace-ve-eval vs_shadow)))

  (test "After DropView bare symbol passes through unchanged"
        'vs_dropped
        (begin
          (ve-ve-eval (DefineView vs_dropped (Table (A 1 2 3))))
          (ve-ve-eval (DropView vs_dropped))
          (ve-ve-eval vs_dropped)))

  (test "After ClearViews bare symbol passes through unchanged"
        'vs_cleared
        (begin
          (ve-ve-eval (DefineView vs_cleared (Table (A 1 2 3))))
          (ve-ve-eval (ClearViews))
          (ve-ve-eval vs_cleared)))

  (test "Redefined view bare symbol reflects new definition"
        '(Table (A 99))
        (begin
          (ve-ace-ve-eval (DefineView vs_redef (Table (A 1 2 3))))
          (ve-ace-ve-eval (DefineView vs_redef (Table (A 99))))
          (ve-ace-ve-eval vs_redef))))


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

  (test "Eager table is not affected by column pruning, wrapped in ByName"
        '(Project (ByName cp5_tbl) cp5_a)
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable cp5_tbl "/data/cp5.tbl" "/lib/loader.so" #f cp5_a cp5_b cp5_c))
          (ve-eval (Project cp5_tbl cp5_a))))

  ;; --- Regression: a computed/aliased projection (As(expr, alias)) must gather the real base
  ;; --- column(s) the expression references, and only those - not the operator names wrapping
  ;; --- it (Year/Timestamp) and not the new alias name itself, since extractColumnsFromExpr's
  ;; --- visitor only recurses into dynamic arguments, never a ComplexExpression's head. ---

  (test "Computed alias expression gathers only its underlying base column, not operator names"
        '(Project (Gather "/data/cp6.tbl" "/lib/loader.so" (Table) (List cp6_orderdate))
                  (As (Year (Timestamp cp6_orderdate)) cp6_year))
        (begin
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable cp6_tbl "/data/cp6.tbl" "/lib/loader.so" #t cp6_orderdate cp6_other))
          (ve-eval (Project cp6_tbl (As (Year (Timestamp cp6_orderdate)) cp6_year))))))


(test-group "Join canonicalisation - rewriter"

  (ve-ve-eval (ClearViews))
  (ve-ve-eval (ClearTables))

  ;; --- Side-swap canonicalisation for INNER joins ---

  ;; A matched view's own definition is resolved by ACE alone (see the ve-ve-eval
  ;; comment near the top of this file), so under this no-ACE pipeline it stays
  ;; opaque as (CacheRef viewName Pending) - what these tests check is which view
  ;; matched and what residual (if any) got built around that reference.

  (test "Inner join with swapped sides still matches view"
        '(CacheRef CUSTOMER_ORDERS_VIEW Pending)
        (begin
          (ve-ve-eval (RegisterTable customers "/data/customers.tbl" "/lib/loader.so" #f customer_id customer_name))
          (ve-ve-eval (RegisterTable orders "/data/orders.tbl" "/lib/loader.so" #f order_id order_customer_id))
          (ve-ve-eval (DefineView CUSTOMER_ORDERS_VIEW
              (Join customers orders (Equal customer_id order_customer_id))))
          ;; query issued with sides physically reversed
          (ve-ve-eval (Join orders customers (Equal order_customer_id customer_id)))))

  ;; --- Predicate-pair order independence within a fixed side ---

  (test "Multi-predicate inner join matches regardless of predicate order"
        '(CacheRef SHIPMENT_CONTAINER_VIEW Pending)
        (begin
          (ve-ve-eval (RegisterTable shipments "/data/shipments.tbl" "/lib/loader.so" #f shipment_port shipment_date))
          (ve-ve-eval (RegisterTable containers "/data/containers.tbl" "/lib/loader.so" #f container_port container_date))
          (ve-ve-eval (DefineView SHIPMENT_CONTAINER_VIEW
              (Join shipments containers (Equal shipment_port container_port) (Equal shipment_date container_date))))
          ;; query lists the two equi-predicates in the opposite order
          (ve-ve-eval (Join shipments containers (Equal shipment_date container_date) (Equal shipment_port container_port)))))

  ;; --- Combined: both swapped sides AND swapped predicate order at once ---

  (test "Inner join matches with both sides and predicate order swapped"
        '(CacheRef EMPLOYEE_DEPT_VIEW Pending)
        (begin
          (ve-ve-eval (RegisterTable employees "/data/employees.tbl" "/lib/loader.so" #f employee_dept_x employee_dept_y))
          (ve-ve-eval (RegisterTable departments "/data/departments.tbl" "/lib/loader.so" #f dept_x dept_y))
          (ve-ve-eval (DefineView EMPLOYEE_DEPT_VIEW
              (Join employees departments (Equal employee_dept_x dept_x) (Equal employee_dept_y dept_y))))
          (ve-ve-eval (Join departments employees (Equal dept_y employee_dept_y) (Equal dept_x employee_dept_x)))))

  ;; --- Regression: LEFT/ANTI joins must NOT be side-swapped ---

  (test "LeftJoin with swapped sides does not match (semantics differ)"
        '(LeftJoin (ByName returns) (ByName products) (Equal return_product_id product_id))
        (begin
          (ve-ve-eval (RegisterTable products "/data/products.tbl" "/lib/loader.so" #f product_id product_name))
          (ve-ve-eval (RegisterTable returns "/data/returns.tbl" "/lib/loader.so" #f return_id return_product_id))
          (ve-ve-eval (DefineView PRODUCT_RETURNS_VIEW (LeftJoin products returns (Equal product_id return_product_id))))
          ;; sides reversed relative to the view - must remain unrewritten (passes through as-is)
          (ve-ve-eval (LeftJoin returns products (Equal return_product_id product_id)))))

  ;; --- Sanity: unmatched inner join (different tables) must not spuriously match ---

  (test "Inner join over unrelated tables does not match unrelated view"
        '(Join (ByName suppliers) (ByName warehouses) (Equal supplier_id warehouse_supplier_id))
        (begin
          (ve-ve-eval (RegisterTable suppliers "/data/suppliers.tbl" "/lib/loader.so" #f supplier_id))
          (ve-ve-eval (RegisterTable warehouses "/data/warehouses.tbl" "/lib/loader.so" #f warehouse_supplier_id))
          (ve-ve-eval (Join suppliers warehouses (Equal supplier_id warehouse_supplier_id)))))

  ;; --- View-on-view dependency chain, exercises recursive expandSignature/cache path ---

  (test "Inner join matches through a nested view dependency, sides and predicates reversed"
        '(CacheRef CUSTOMER_ORDERS_PAYMENTS_L2 Pending)
        (begin
          (ve-ve-eval (RegisterTable payments "/data/payments.tbl" "/lib/loader.so" #f payment_id payment_customer_id))
          (ve-ve-eval (DefineView CUSTOMER_ORDERS_L1
              (Join customers orders (Equal customer_id order_customer_id))))
          (ve-ve-eval (DefineView CUSTOMER_ORDERS_PAYMENTS_L2
              (Join (QueryView CUSTOMER_ORDERS_L1 Defer) payments (Equal order_customer_id payment_customer_id))))
          ;; raw query, no embedded QueryView anywhere — sides reversed relative to the view
          (ve-ve-eval (Join payments (Join customers orders (Equal customer_id order_customer_id))
                         (Equal payment_customer_id order_customer_id)))))

  ;; --- Mixed equi-join + residual (non-equi) predicate ---

  (test "Inner join with mixed equi + residual predicate matches regardless of predicate order"
        '(CacheRef PRODUCT_RETURNS_FILTERED_VIEW Pending)
        (begin
          (ve-ve-eval (DefineView PRODUCT_RETURNS_FILTERED_VIEW
              (Join products returns (Equal product_id return_product_id) (Greater product_price 5))))
          ;; query lists the residual predicate before the equi-predicate
          (ve-ve-eval (Join products returns (Greater product_price 5) (Equal product_id return_product_id))))))


(test-group "Partial rewriting - residual Filter/Project"

  (ve-ve-eval (ClearViews))
  (ve-ve-eval (ClearTables))

  (ve-ve-eval (RegisterTable pr_products "/data/pr_products.tbl" "/lib/loader.so" #f
             pr_product_id pr_product_name pr_product_price))
  (ve-ve-eval (RegisterTable pr_returns "/data/pr_returns.tbl" "/lib/loader.so" #f
             pr_return_id pr_return_product_id))

  ;; A matched view's own definition is resolved by ACE alone, so under this no-ACE
  ;; pipeline it stays opaque as (CacheRef viewName Pending) - see the ve-ve-eval
  ;; comment near the top of this file. What these tests check is the residual
  ;; Filter/Project VE1 builds around that reference.

  (test "Predicate-only residual: extra Filter applied on top of resolved view"
        '(Filter (CacheRef PR_JOIN_VIEW Pending) (Greater pr_product_price 5))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView PR_JOIN_VIEW
              (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))))
          (ve-ve-eval (Filter
              (Join pr_returns pr_products (Equal pr_return_product_id pr_product_id))
              (Greater pr_product_price 5)))))

  (test "Projection-only residual: view over-projects, query narrows via residual Project"
        '(Project (CacheRef PR_WIDE_VIEW Pending) pr_product_id)
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView PR_WIDE_VIEW
              (Project (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
                        pr_product_id pr_product_name pr_product_price pr_return_id)))
          (ve-ve-eval (Project
              (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
              pr_product_id))))

  (test "Combined residual: Filter innermost, Project outermost, wrapping resolved view"
        '(Project (Filter (CacheRef PR_PLAIN_VIEW Pending) (Greater pr_product_price 5)) pr_product_id)
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView PR_PLAIN_VIEW
              (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))))
          (ve-ve-eval (Project
              (Filter
                (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
                (Greater pr_product_price 5))
              pr_product_id))))

  (test "Not rewritable: view missing a projected column leaves query unchanged"
        '(Project (Join (ByName pr_products) (ByName pr_returns) (Equal pr_product_id pr_return_product_id))
                   pr_product_id pr_product_price)
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView PR_NARROW_VIEW
              (Project (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
                        pr_product_id pr_return_id)))
          (ve-ve-eval (Project
              (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
              pr_product_id pr_product_price))))
              
  (test "Not rewritable: view predicate absent from query is not silently applied"
        '(Join (ByName pr_products) (ByName pr_returns) (Equal pr_product_id pr_return_product_id))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView PR_FILTERED_VIEW
              (Filter
                (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))
                (Greater pr_product_price 5))))
          (ve-ve-eval (Join pr_products pr_returns (Equal pr_product_id pr_return_product_id))))))


(test-group "Domain predicate semantics"

  (ve-ve-eval (ClearViews))
  (ve-ve-eval (ClearTables))

  (ve-ve-eval (RegisterTable dp_products "/data/dp_products.tbl" "/lib/loader.so" #f
             dp_product_id dp_product_name dp_product_price))
  (ve-ve-eval (RegisterTable dp_returns "/data/dp_returns.tbl" "/lib/loader.so" #f
             dp_return_id dp_return_product_id))

  ;; A matched view's own definition is resolved by ACE alone, so under this no-ACE
  ;; pipeline it stays opaque as (CacheRef viewName Pending) - see the ve-ve-eval
  ;; comment near the top of this file. What these tests check is which view
  ;; matched and what residual (if any) got built around that reference.

  ;; --- View domain strictly weaker than query domain: must match with residual ---

  (test "View with Greater(price,50) matches query needing Greater(price,100), residual added"
        '(Filter (CacheRef DP_WEAK_VIEW Pending) (Greater dp_product_price 100))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView DP_WEAK_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (Greater dp_product_price 50))))
          (ve-ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 100)))))

  ;; --- View domain strictly stronger than query domain: must NOT match ---

  (test "View with Greater(price,100) does not cover query needing Greater(price,50)"
        '(Filter (Join (ByName dp_products) (ByName dp_returns) (Equal dp_product_id dp_return_product_id))
                 (Greater dp_product_price 50))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView DP_STRONG_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (Greater dp_product_price 100))))
          (ve-ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 50)))))

  ;; --- Exact domain match: no residual should be added ---

  (test "View with exact same domain predicate as query needs no residual"
        '(CacheRef DP_EXACT_VIEW Pending)
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView DP_EXACT_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (Greater dp_product_price 50))))
          (ve-ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 50)))))

  ;; --- Between covered by an equivalent view built from two comparisons ---

  (test "View with Greater+Less matches query needing a narrower Between"
        '(Filter (CacheRef DP_RANGE_VIEW Pending) (Between dp_product_price 20 30))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView DP_RANGE_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (And (Greater dp_product_price 10) (Less dp_product_price 50)))))
          (ve-ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Between dp_product_price 20 30)))))

  ;; --- Unrestricted view column vs query domain: usable but residual still needed ---

  (test "View unrestricted on price still needs residual Filter for query's Greater"
        '(Filter (CacheRef DP_PLAIN_VIEW Pending) (Greater dp_product_price 5))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView DP_PLAIN_VIEW
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))))
          (ve-ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 5)))))

  ;; --- Single-column Or: real domain union, should match a subset request ---

  (test "View with Or on same column (union) matches a query needing a subset range"
        '(Filter (CacheRef DP_OR_VIEW Pending) (Greater dp_product_price 5))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView DP_OR_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (Or (Less dp_product_price 10) (Greater dp_product_price 5)))))
          (ve-ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 5)))))

  ;; --- Cross-column Or: opaque fallback, only exact same expression should match ---

  (test "View with cross-column Or matches only exact same Or expression in query"
        '(CacheRef DP_CROSS_OR_VIEW Pending)
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView DP_CROSS_OR_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (Or (Greater dp_product_price 5) (Greater dp_return_id 10)))))
          (ve-ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Or (Greater dp_product_price 5) (Greater dp_return_id 10))))))

  (test "View with cross-column Or does not match a differently-shaped query predicate"
        '(Filter (Join (ByName dp_products) (ByName dp_returns) (Equal dp_product_id dp_return_product_id))
                 (Greater dp_product_price 5))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView DP_CROSS_OR_VIEW2
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (Or (Greater dp_product_price 5) (Greater dp_return_id 10)))))
          (ve-ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 5)))))

  ;; --- NotEqual: split-range domain ---

  (test "View with matching NotEqual needs no residual"
        '(CacheRef DP_NOTEQUAL_VIEW Pending)
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView DP_NOTEQUAL_VIEW
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (NotEqual dp_product_price 5))))
          (ve-ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (NotEqual dp_product_price 5)))))

  (test "View with NotEqual(5) covers query needing Greater(10) via subset range, residual added"
        '(Filter (CacheRef DP_NOTEQUAL_VIEW2 Pending) (Greater dp_product_price 10))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView DP_NOTEQUAL_VIEW2
              (Filter
                (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
                (NotEqual dp_product_price 5))))
          (ve-ve-eval (Filter
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))
              (Greater dp_product_price 10)))))

  ;; --- LEFT JOIN with a pre-filtered right (destructive) side: rewriting must be blocked ---

  (test "View is a LeftJoin with filtered right side - query against it is not rewritten"
        '(LeftJoin (ByName dp_products)
             (Filter (ByName dp_returns) (Greater dp_return_id 100))
             (Equal dp_product_id dp_return_product_id))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView DP_LEFTJOIN_VIEW
              (LeftJoin dp_products
                 (Filter dp_returns (Greater dp_return_id 100))
                 (Equal dp_product_id dp_return_product_id))))
          (ve-ve-eval (LeftJoin dp_products
                       (Filter dp_returns (Greater dp_return_id 100))
                       (Equal dp_product_id dp_return_product_id)))))

  ;; --- Regression: exact base-table/join match but unrestricted domain column must still residual ---

  (test "Regression: unrestricted view on filtered column always produces residual, never bare QueryView"
        '(Filter (CacheRef DP_REGRESSION_VIEW Pending) (Greater dp_product_price 5))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView DP_REGRESSION_VIEW
              (Join dp_products dp_returns (Equal dp_product_id dp_return_product_id))))
          (ve-ve-eval (Filter
              (Join dp_returns dp_products (Equal dp_return_product_id dp_product_id))
              (Greater dp_product_price 5))))))


(test-group "Domain predicate on a column projected out of the view"

  (ve-ve-eval (ClearViews))
  (ve-ve-eval (ClearTables))

  (ve-ve-eval (RegisterTable cd_products "/data/cd_products.tbl" "/lib/loader.so" #f
             cd_product_id cd_product_name cd_product_price))

  ;; --- View filters on a column it then projects out: a stronger query predicate on that
  ;; --- column can't be re-applied as a residual filter, since the column is gone from the
  ;; --- view's output. Must NOT rewrite. ---

  (test "Regression: view predicate weaker but on a column projected out is not rewritable"
        '(Project (Filter (ByName cd_products) (Greater cd_product_price 100))
                  cd_product_id cd_product_name)
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView CD_DROPPED_VIEW
              (Project (Filter cd_products (Greater cd_product_price 50))
                       cd_product_id cd_product_name)))
          (ve-ve-eval (Project
              (Filter cd_products (Greater cd_product_price 100))
              cd_product_id cd_product_name))))

  ;; --- Control: same shape, but the view keeps the filtered column in its projection, so the
  ;; --- stronger query predicate can be applied as a residual Filter, with a residual Project
  ;; --- on top to trim back down to what the query needs. ---

  ;; A matched view's own definition is resolved by ACE alone, so under this no-ACE
  ;; pipeline it stays opaque as (CacheRef viewName Pending) - see the ve-ve-eval
  ;; comment near the top of this file.

  (test "View predicate weaker on a column still present in the projection is rewritable"
        '(Project (Filter (CacheRef CD_KEPT_VIEW Pending) (Greater cd_product_price 100)) cd_product_id)
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView CD_KEPT_VIEW
              (Project (Filter cd_products (Greater cd_product_price 50))
                       cd_product_id cd_product_name cd_product_price)))
          (ve-ve-eval (Project
              (Filter cd_products (Greater cd_product_price 100))
              cd_product_id)))))


(test-group "Opaque predicate on a column projected out of the view"

  (ve-ve-eval (ClearViews))
  (ve-ve-eval (ClearTables))

  (ve-ve-eval (RegisterTable op_products "/data/op_products.tbl" "/lib/loader.so" #f
             op_id op_name op_price))

  ;; --- View has no opaque predicate on a column it projects out; the query's opaque predicate
  ;; --- on that column would need a residual filter, but the column is gone from the view's
  ;; --- output. Must NOT rewrite. ---

  (test "Regression: query's opaque predicate on a column projected out is not rewritable"
        '(Project (Filter (ByName op_products) (IsValid op_name)) op_id op_price)
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView OP_DROPPED_VIEW
              (Project op_products op_id op_price)))
          (ve-ve-eval (Project
              (Filter op_products (IsValid op_name))
              op_id op_price))))

  ;; --- Control: same shape, but the view keeps the predicated column in its projection, so the
  ;; --- query's opaque predicate can be applied as a residual Filter. ---

  ;; A matched view's own definition is resolved by ACE alone, so under this no-ACE
  ;; pipeline it stays opaque as (CacheRef viewName Pending) - see the ve-ve-eval
  ;; comment near the top of this file.

  (test "Query's opaque predicate on a column still present in the projection is rewritable"
        '(Project (Filter (CacheRef OP_KEPT_VIEW Pending) (IsValid op_name)) op_id)
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView OP_KEPT_VIEW
              (Project op_products op_id op_name op_price)))
          (ve-ve-eval (Project
              (Filter op_products (IsValid op_name))
              op_id)))))


(test-group "View missing a table/join the query needs"

  (ve-ve-eval (ClearViews))
  (ve-ve-eval (ClearTables))

  (ve-ve-eval (RegisterTable jp_a "/data/jp_a.tbl" "/lib/loader.so" #f jp_a_id jp_a_val))
  (ve-ve-eval (RegisterTable jp_b "/data/jp_b.tbl" "/lib/loader.so" #f jp_b_id))

  ;; --- Regression: a view over only one side of a join must not be used to answer a query that
  ;; --- joins in a second table - there's no residual Join, so the join (and its row-filtering
  ;; --- effect) would silently vanish from the rewritten query. Must NOT rewrite. ---

  (test "Regression: view missing a joined table is not rewritable, even projecting only the covered side"
        '(Project (Join (ByName jp_a) (ByName jp_b) (Equal jp_a_id jp_b_id)) jp_a_id jp_a_val)
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView JP_A_ONLY (Project jp_a jp_a_id jp_a_val)))
          (ve-ve-eval (Project
              (Join jp_a jp_b (Equal jp_a_id jp_b_id))
              jp_a_id jp_a_val))))

  ;; --- Control: same shape, but the view already performs the join - must rewrite. ---

  ;; A matched view's own definition is resolved by ACE alone, so under this no-ACE
  ;; pipeline it stays opaque as (CacheRef viewName Pending) - see the ve-ve-eval
  ;; comment near the top of this file.

  (test "View covering both joined tables is rewritable"
        '(CacheRef JP_BOTH_VIEW Pending)
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView JP_BOTH_VIEW
              (Project (Join jp_a jp_b (Equal jp_a_id jp_b_id)) jp_a_id jp_a_val)))
          (ve-ve-eval (Project
              (Join jp_a jp_b (Equal jp_a_id jp_b_id))
              jp_a_id jp_a_val)))))


(test-group "Domain partial credit reflects real interval tightness"

  (ve-ve-eval (ClearViews))
  (ve-ve-eval (ClearTables))

  (ve-ve-eval (RegisterTable pc_lineitem "/data/pc_lineitem.tbl" "/lib/loader.so" #f
             pc_l_orderkey pc_l_quantity pc_l_discount))

  ;; --- Regression: a view with real (even if non-exact) predicates on every column the query
  ;; --- filters on must beat a view that only predicates one of those columns and has no
  ;; --- predicate at all on the other - "no predicate" must score strictly worse than "a real,
  ;; --- if wide, predicate", and a tighter range must score better than a wider one. Before the
  ;; --- fix these both scored identically (partial credit silently truncated to 0 via a size_t
  ;; --- accumulator), producing ties broken by iteration order/cache status instead of which
  ;; --- view actually does less residual filtering work. ---

  ;; A matched view's own definition is resolved by ACE alone, so under this no-ACE
  ;; pipeline it stays opaque as (CacheRef viewName Pending) - see the ve-ve-eval
  ;; comment near the top of this file. What matters here is which view won.

  (test "View with tighter, fuller predicates beats a view with a wide predicate and a gap"
        '(Filter (CacheRef PC_TIGHT_VIEW Pending) (Between pc_l_quantity 15 20))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView PC_WIDE_VIEW
              (Filter pc_lineitem (Between pc_l_quantity 1 50))))
          (ve-ve-eval (DefineView PC_TIGHT_VIEW
              (Filter pc_lineitem (And (Between pc_l_quantity 10 30) (Between pc_l_discount 2 4)))))
          (ve-ve-eval (Filter pc_lineitem
              (And (Between pc_l_quantity 15 20) (Between pc_l_discount 2 4)))))))


(test-group "Opaque predicate on Project-aliased columns must not vanish from the signature"

  (ve-ve-eval (ClearViews))
  (ve-ve-eval (ClearTables))

  (ve-ve-eval (RegisterTable eb_orders "/data/eb_orders.tbl" "/lib/loader.so" #f
             eb_supp_nation eb_cust_nation eb_qty))

  ;; --- Regression: an Or predicate spanning two Project-aliased columns (supp_nation/cust_nation
  ;; --- come from As(...), not real registered table columns) must still be registered as an
  ;; --- opaque predicate keyed by those alias symbols. Before the fix, extractColumnsFromExpr only
  ;; --- recognised symbols already in columnRegistry (base-table schema columns), so the alias
  ;; --- names came back empty, the whole Or predicate was silently dropped from the query's
  ;; --- signature, and a view with no filter at all scored a perfect match - the filter vanished
  ;; --- entirely from the rewritten query instead of surviving as a residual. ---

  ;; A matched view's own definition is resolved by ACE alone, so under this no-ACE
  ;; pipeline it stays opaque as (CacheRef viewName Pending) - see the ve-ve-eval
  ;; comment near the top of this file. What matters here is that the Or predicate
  ;; survives as a residual Filter instead of vanishing (i.e. instead of a bare,
  ;; unwrapped CacheRef).

  (test "Or predicate on aliased columns survives as a residual filter, not silently dropped"
        '(Filter (CacheRef EB_DECOY_VIEW Pending)
          (Or (And (Equal supp_nation 1) (Equal cust_nation 2))
              (And (Equal supp_nation 2) (Equal cust_nation 1))))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView EB_DECOY_VIEW
              (Project eb_orders eb_qty (As eb_supp_nation supp_nation) (As eb_cust_nation cust_nation))))
          (ve-ve-eval (Filter
              (Project eb_orders eb_qty (As eb_supp_nation supp_nation) (As eb_cust_nation cust_nation))
              (Or (And (Equal supp_nation 1) (Equal cust_nation 2))
                  (And (Equal supp_nation 2) (Equal cust_nation 1))))))))


(test-group "intersectMerge null-pointer regression"

  (ve-eval (ClearViews))
  (ve-eval (ClearTables))

  (ve-eval (RegisterTable nd_a "/data/nd_a.tbl" "/lib/loader.so" #f nd_id nd_val))
  (ve-eval (RegisterTable nd_b "/data/nd_b.tbl" "/lib/loader.so" #f nd_id nd_other))

  ;; --- Regression: a Join whose two branches both produce an output column with the same
  ;; --- symbol (nd_id), where one side is a bare pass-through Project (nullptr in
  ;; --- projectedColumns) and the other is an As(...) alias landing on that same symbol (a real
  ;; --- pointer). intersectMerge used to dereference both sides unconditionally when comparing
  ;; --- the two branches' projections for a conflict, segfaulting whenever either side was a bare
  ;; --- pass-through. This doesn't even need a registered view - walking the query itself to
  ;; --- build its signature was enough to crash the process. ---

  (test "Join with a bare pass-through and an As(...) alias on the same output column doesn't crash"
        '(Join (Project (ByName nd_a) nd_id) (Project (ByName nd_b) (As nd_id nd_id)) (Equal nd_id nd_id))
        (ve-eval (Join
            (Project nd_a nd_id)
            (Project nd_b (As nd_id nd_id))
            (Equal nd_id nd_id)))))


(test-group "scoreView projection null/non-null mismatch"

  (ve-ve-eval (ClearViews))
  (ve-ve-eval (ClearTables))

  (ve-ve-eval (RegisterTable pm_orders "/data/pm_orders.tbl" "/lib/loader.so" #f
             pm_price pm_qty pm_revenue))

  ;; --- Regression: a view that passes the base column pm_revenue straight through (nullptr in
  ;; --- projectedColumns) must not be treated as satisfying a query that wants a column with the
  ;; --- same name computed as price*qty (a real pointer in projectedColumns). The old check
  ;; --- `it->second && queryExpr && ...` short-circuited to "no mismatch" whenever either side was
  ;; --- null, so the raw base column silently stood in for the computed value. Must NOT rewrite. ---

  (test "View's raw pass-through column does not satisfy a query needing it computed"
        '(Project (ByName pm_orders) (As (Multiply pm_price pm_qty) pm_revenue))
        (begin
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView PM_DECOY_VIEW
              (Project pm_orders pm_revenue)))
          (ve-ve-eval (Project pm_orders (As (Multiply pm_price pm_qty) pm_revenue))))))


(test-group "Lite mode (SetEngineMode)"

  ;; Lite mode bypasses both the query rewriter (findRewriting) and the cache (viewCache /
  ;; WithCaches / CacheRef): QueryView and bare-symbol view references just substitute the
  ;; stored definition, recursively, with nothing cached and no rewrite candidates searched
  ;; for. Because no CacheRef/WithCaches wrapper is ever produced in this mode, a single VE
  ;; pass with no ACE is enough to see the fully-substituted result - contrast with Full
  ;; mode's ve-ve-eval/ve-ace-ve-eval requirement (see the comment near the top of this file).

  (ve-eval (ClearViews))
  (ve-eval (ClearTables))
  (ve-eval (SetEngineMode Full)) ;; ensure a known starting mode

  (test "SetEngineMode Lite returns true"
        #t
        (ve-eval (SetEngineMode Lite)))

  (test "SetEngineMode Full returns true"
        #t
        (ve-eval (SetEngineMode Full)))

  (test "SetEngineMode with unknown symbol returns false"
        #f
        (ve-eval (SetEngineMode Bogus)))

  (test "SetEngineMode with no arguments returns false"
        #f
        (ve-eval (SetEngineMode)))

  (test "SetEngineMode with too many arguments returns false"
        #f
        (ve-eval (SetEngineMode Lite Full)))

  (test "SetEngineMode with non-symbol argument returns false"
        #f
        (ve-eval (SetEngineMode 123)))

  (test "SetEngineMode not at top level returns false"
        '(TestWrapper #f)
        (ve-eval (TestWrapper (SetEngineMode Lite))))

  (test "QueryView fully substitutes in a single VE pass in Lite mode"
        '(Table (A 1 2 3))
        (begin
          (ve-eval (SetEngineMode Lite))
          (ve-eval (DefineView LM_SIMPLE (Table (A 1 2 3))))
          (ve-eval (QueryView LM_SIMPLE))))

  (test "QueryView with legacy caching-decision argument is accepted and ignored in Lite mode"
        '(Table (A 1 2 3))
        (begin
          (ve-eval (SetEngineMode Lite))
          (ve-eval (DefineView LM_LEGACY (Table (A 1 2 3))))
          (ve-eval (QueryView LM_LEGACY Admit Standard Content))))

  (test "Bare view symbol substitutes directly with no rewrite metadata needed"
        '(Filter (Table (A 1 2 3)) (Greater A 1))
        (begin
          (ve-eval (SetEngineMode Lite))
          (ve-eval (DefineView LM_BARE (Table (A 1 2 3))))
          (ve-eval (Filter LM_BARE (Greater A 1)))))

  (test "Nested view-on-view substitution fully resolves in a single VE pass"
        '(GroupBy (Filter (Table (A 1 2 3)) (Greater A 1)) (Sum A))
        (begin
          (ve-eval (SetEngineMode Lite))
          (ve-eval (DefineView LM_INNER (Table (A 1 2 3))))
          (ve-eval (DefineView LM_OUTER (Filter (QueryView LM_INNER Defer) (Greater A 1))))
          (ve-eval (GroupBy (QueryView LM_OUTER) (Sum A)))))

  ;; No query rewriter in Lite mode: a freshly-issued query is never rewritten into a
  ;; reference to an existing view, even when its shape matches one exactly.
  (test "Matching query shape is not rewritten to an existing view in Lite mode"
        '(Filter (ByName lm_rw_tbl) (Greater lm_rw_col 5))
        (begin
          (ve-eval (SetEngineMode Lite))
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable lm_rw_tbl "/data/lm_rw.tbl" "/lib/loader.so" #f lm_rw_col))
          (ve-eval (DefineView LM_RW_VIEW (Filter lm_rw_tbl (Greater lm_rw_col 5))))
          (ve-eval (Filter lm_rw_tbl (Greater lm_rw_col 5)))))

  ;; No caching in Lite mode: querying the same view twice re-substitutes the definition
  ;; fresh each time instead of reusing/borrowing a cached value.
  (test "Repeated QueryView of the same view in Lite mode re-substitutes rather than caching"
        '(Table (A 1 2 3))
        (begin
          (ve-eval (SetEngineMode Lite))
          (ve-eval (DefineView LM_REPEAT (Table (A 1 2 3))))
          (ve-eval (QueryView LM_REPEAT))
          (ve-eval (QueryView LM_REPEAT))))

  ;; Table resolution (lazy -> Gather, eager -> ByName) is unaffected by Lite mode; the only
  ;; difference is column pruning doesn't apply since there's no rewriter-built signature to
  ;; prune with, so Gather requests all columns (empty List).
  (test "Lazy table symbol still resolves to Gather in Lite mode without column pruning"
        '(Project (Gather "/data/lm_lazy.tbl" "/lib/loader.so" (Table) (List)) lm_lazy_col)
        (begin
          (ve-eval (SetEngineMode Lite))
          (ve-eval (ClearTables))
          (ve-eval (RegisterTable lm_lazy_tbl "/data/lm_lazy.tbl" "/lib/loader.so" #t lm_lazy_col))
          (ve-eval (Project lm_lazy_tbl lm_lazy_col))))

  ;; DefineView bookkeeping (dependency graph, cycle detection) is registry integrity, not
  ;; part of the rewriter or the cache, so it stays active in Lite mode too.
  (test "Cycle detection still blocks a cyclic DefineView in Lite mode"
        #f
        (begin
          (ve-eval (SetEngineMode Lite))
          (ve-eval (DefineView LM_CYCA (QueryView LM_CYCB Defer)))
          (ve-eval (DefineView LM_CYCB (QueryView LM_CYCA Defer)))))

  (test "QueryView on unknown view still throws in Lite mode"
        '(ErrorWhenEvaluatingExpression (||) "View not found: LM_NONEXISTENT")
        (begin
          (ve-eval (SetEngineMode Lite))
          (ve-eval (QueryView LM_NONEXISTENT))))

  ;; Full mode's caching/rewriting resumes exactly as before once switched back - a fresh
  ;; cache-miss QueryView opaquely returns CacheRef, same as any other Full-mode test (see
  ;; the ve-ve-eval comment near the top of this file).
  (test "Switching back to Full mode restores caching/rewriting via CacheRef"
        '(CacheRef LM_BACKTOFULL Pending)
        (begin
          (ve-ve-eval (SetEngineMode Lite))
          (ve-ve-eval (ClearViews))
          (ve-ve-eval (DefineView LM_BACKTOFULL (Table (A 1 2 3))))
          (ve-ve-eval (SetEngineMode Full))
          (ve-ve-eval (QueryView LM_BACKTOFULL Defer))))

  (ve-eval (SetEngineMode Full))) ;; leave the engine in its default mode for any tests that follow