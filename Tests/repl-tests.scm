(import (scheme base)
        (chibi test)
        (BOSS))

(define-syntax ve-eval
  (syntax-rules ()
    ((ve-eval query)
     (boss-eval (EvaluateInEngines
       (List "/mnt/apps/lazy-loading/ViewEngine/build/libViewEngine.so"
             "/mnt/apps/lazy-loading/ViewEngine/build/libViewEngine.so")
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
        '(ErrorWhenEvaluatingExpression (||) "QueryView requires exactly 1 symbol argument")
        (ve-eval (QueryView)))

  (test "QueryView with too many arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView requires exactly 1 symbol argument")
        (begin
          (ve-eval (DefineView V1 (Table (A 1))))
          (ve-eval (DefineView V2 (Table (A 2))))
          (ve-eval (QueryView V1 V2))))

  (test "QueryView on unknown view throws"
        '(ErrorWhenEvaluatingExpression (||) "View not found: NONEXISTENT")
        (ve-eval (QueryView NONEXISTENT)))

  (test "QueryView with integer argument throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView argument must be a symbol")
        (ve-eval (QueryView 123)))

  (test "QueryView with expression argument throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView argument must be a symbol")
        (ve-eval (QueryView (Table (A 1 2 3))))))


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

  (test "DropView on non-existent view returns true"
        #t
        (ve-eval (DropView NONEXISTENT_DROP)))

  (test "DropView removes view from registry"
        '(ErrorWhenEvaluatingExpression (||) "View not found: DROP2")
        (begin
          (ve-eval (DefineView DROP2 (Table (A 1 2 3))))
          (ve-eval (DropView DROP2))
          (ve-eval (QueryView DROP2))))

  (test "DropView with no arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "DropView requires exactly 1 symbol argument")
        (ve-eval (DropView)))

  (test "DropView with too many arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "DropView requires exactly 1 symbol argument")
        (begin
          (ve-eval (DefineView DROP3 (Table (A 1 2 3))))
          (ve-eval (DefineView DROP4 (Table (A 1 2 3))))
          (ve-eval (DropView DROP3 DROP4))))

  (test "DropView with integer argument throws"
        '(ErrorWhenEvaluatingExpression (||) "DropView argument must be a symbol")
        (ve-eval (DropView 123)))

  (test "DropView with expression argument throws"
        '(ErrorWhenEvaluatingExpression (||) "DropView argument must be a symbol")
        (ve-eval (DropView (Table (A 1 2 3)))))

  (test "DropView on view currently being evaluated blocked at define time"
      #f
      (ve-eval (DefineView DROP5 (Filter (DropView DROP5) (Greater A 1)))))

  (test "DropView succeeds after previously failing mid-evaluation"
        #t
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

  (test "ClearViews with arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "ClearViews does not take any arguments")
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