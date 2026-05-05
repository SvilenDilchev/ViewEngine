(import (scheme base)
        (chibi test)
        (BOSS))

(boss-eval (SetDefaultEnginePipeline
    "/mnt/apps/lazy-loading/ViewEngine/build/libViewEngine.so"))

(test-group "DefineView"

  (test "DefineView returns true on success"
        #t
        (boss-eval (DefineView MY_VIEW (Filter (Table (A 1 2 3)) (Greater A 1)))))

  (test "DefineView overwrites existing view"
        #t
        (begin
          (boss-eval (DefineView OVERWRITE (Table (A 1 2))))
          (boss-eval (DefineView OVERWRITE (Table (A 10 20)))))))

(test-group "DefineView failures"

  (test "DefineView with no arguments returns false"
        #f
        (boss-eval (DefineView)))

  (test "DefineView with only name and no body returns false"
        #f
        (boss-eval (DefineView MY_VIEW_NO_BODY)))

  (test "DefineView with too many arguments returns false"
        #f
        (boss-eval (DefineView TOO_MANY (Table (A 1)) (Table (B 2)))))

  (test "DefineView with integer as name returns false"
        #f
        (boss-eval (DefineView 123 (Table (A 1 2 3)))))

  (test "DefineView with string as name returns false"
        #f
        (boss-eval (DefineView "my_view" (Table (A 1 2 3)))))

  (test "DefineView with expression as name returns false"
        #f
        (boss-eval (DefineView (Table (A 1)) (Table (A 1 2 3))))))

(test-group "QueryView"

  (test "QueryView resolves to stored expression"
        '(Filter (Table (A 1 2 3)) (Greater A 1))
        (begin
          (boss-eval (DefineView SIMPLE (Filter (Table (A 1 2 3)) (Greater A 1))))
          (boss-eval (QueryView SIMPLE))))

  (test "QueryView returns latest definition after overwrite"
        '(Table (A 10 20))
        (begin
          (boss-eval (DefineView OVERWRITE2 (Table (A 1 2))))
          (boss-eval (DefineView OVERWRITE2 (Table (A 10 20))))
          (boss-eval (QueryView OVERWRITE2))))

  (test "QueryView same view twice returns same expression"
        '(Table (A 1 2 3))
        (begin
          (boss-eval (DefineView REPEATED (Table (A 1 2 3))))
          (boss-eval (QueryView REPEATED))
          (boss-eval (QueryView REPEATED)))))

(test-group "QueryView errors"

  (test "QueryView with no arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView requires exactly 1 symbol argument")
        (boss-eval (QueryView)))

  (test "QueryView with too many arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView requires exactly 1 symbol argument")
        (begin
          (boss-eval (DefineView V1 (Table (A 1))))
          (boss-eval (DefineView V2 (Table (A 2))))
          (boss-eval (QueryView V1 V2))))

  (test "QueryView on unknown view throws"
        '(ErrorWhenEvaluatingExpression (||) "View not found: NONEXISTENT")
        (boss-eval (QueryView NONEXISTENT)))

  (test "QueryView with integer argument throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView argument must be a symbol")
        (boss-eval (QueryView 123)))

  (test "QueryView with expression argument throws"
        '(ErrorWhenEvaluatingExpression (||) "QueryView argument must be a symbol")
        (boss-eval (QueryView (Table (A 1 2 3))))))

(test-group "Nested views"

  (test "QueryView inside stored expression gets resolved"
        '(Filter (Table (A 1 2 3)) (Greater A 1))
        (begin
          (boss-eval (DefineView INNER (Table (A 1 2 3))))
          (boss-eval (DefineView OUTER (Filter (QueryView INNER) (Greater A 1))))
          (boss-eval (QueryView OUTER))))

  (test "Three levels deep"
        '(GroupBy (Filter (Table (A 1 2 3)) (Greater A 1)) (Sum A))
        (begin
          (boss-eval (DefineView LEVEL1 (Table (A 1 2 3))))
          (boss-eval (DefineView LEVEL2 (Filter (QueryView LEVEL1) (Greater A 1))))
          (boss-eval (DefineView LEVEL3 (GroupBy (QueryView LEVEL2) (Sum A))))
          (boss-eval (QueryView LEVEL3)))))

(test-group "Nested DefineView"

  ;; INNER2 is stored unevaluated inside OUTER3's body — querying INNER2 before
  ;; OUTER3 throws because INNER2 has never been registered
  (test "Querying inner before outer throws"
        '(ErrorWhenEvaluatingExpression (||) "View not found: INNER2")
        (begin
          (boss-eval (DefineView OUTER3
              (Filter (DefineView INNER2 (Table (A 1 2 3))) (Greater A 1))))
          (boss-eval (QueryView INNER2))))

  ;; Querying OUTER4 registers INNER3 as a side effect (returns bool in place of DefineView)
  ;; then querying INNER3 succeeds
  (test "Querying outer registers inner as side effect"
        '(Table (A 1 2 3))
        (begin
          (boss-eval (DefineView OUTER4
              (Filter (DefineView INNER3 (Table (A 1 2 3))) (Greater A 1))))
          (boss-eval (QueryView OUTER4))
          (boss-eval (QueryView INNER3)))))

(test-group "QueryView inside expressions"

  (test "QueryView as argument to Filter passes through resolved"
        '(Filter (Table (A 1 2 3)) (Greater A 1))
        (begin
          (boss-eval (DefineView BASE (Table (A 1 2 3))))
          (boss-eval (Filter (QueryView BASE) (Greater A 1)))))

  (test "QueryView as argument to GroupBy passes through resolved"
        '(GroupBy (Table (A 1 2 3)) (Sum A))
        (begin
          (boss-eval (DefineView BASE2 (Table (A 1 2 3))))
          (boss-eval (GroupBy (QueryView BASE2) (Sum A)))))

  (test "QueryView as argument to Project passes through resolved"
        '(Project (Table (A 1 2 3) (B 4 5 6)) A)
        (begin
          (boss-eval (DefineView BASE3 (Table (A 1 2 3) (B 4 5 6))))
          (boss-eval (Project (QueryView BASE3) A)))))

(test-group "Circular view detection"

  (test "Self-reference throws"
        '(ErrorWhenEvaluatingExpression (||) "Circular view dependency detected: SELF")
        (begin
          (boss-eval (DefineView SELF (QueryView SELF)))
          (boss-eval (QueryView SELF))))

  (test "Direct cycle A -> B -> A throws"
        '(ErrorWhenEvaluatingExpression (||) "Circular view dependency detected: CYCA")
        (begin
          (boss-eval (DefineView CYCA (QueryView CYCB)))
          (boss-eval (DefineView CYCB (QueryView CYCA)))
          (boss-eval (QueryView CYCA))))

  (test "Three-step cycle A -> B -> C -> A throws"
        '(ErrorWhenEvaluatingExpression (||) "Circular view dependency detected: TRIIA")
        (begin
          (boss-eval (DefineView TRIIA (QueryView TRIIB)))
          (boss-eval (DefineView TRIIB (QueryView TRIIC)))
          (boss-eval (DefineView TRIIC (QueryView TRIIA)))
          (boss-eval (QueryView TRIIA))))

  (test "Stack is clean after cycle, same view queryable again"
        '(Table (A 1 2 3))
        (begin
          (boss-eval (DefineView CLEAN_A (QueryView CLEAN_B)))
          (boss-eval (DefineView CLEAN_B (QueryView CLEAN_A)))
          (boss-eval (QueryView CLEAN_A))
          (boss-eval (DefineView CLEAN_A (Table (A 1 2 3))))
          (boss-eval (QueryView CLEAN_A)))))

(test-group "DropView"

  (test "DropView returns true on success"
        #t
        (begin
          (boss-eval (DefineView DROP1 (Table (A 1 2 3))))
          (boss-eval (DropView DROP1))))

  (test "DropView on non-existent view returns true"
        #t
        (boss-eval (DropView NONEXISTENT_DROP)))

  (test "DropView removes view from registry"
        '(ErrorWhenEvaluatingExpression (||) "View not found: DROP2")
        (begin
          (boss-eval (DefineView DROP2 (Table (A 1 2 3))))
          (boss-eval (DropView DROP2))
          (boss-eval (QueryView DROP2))))

  (test "DropView with no arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "DropView requires exactly 1 symbol argument")
        (boss-eval (DropView)))

  (test "DropView with too many arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "DropView requires exactly 1 symbol argument")
        (begin
          (boss-eval (DefineView DROP3 (Table (A 1 2 3))))
          (boss-eval (DefineView DROP4 (Table (A 1 2 3))))
          (boss-eval (DropView DROP3 DROP4))))

  (test "DropView with integer argument throws"
        '(ErrorWhenEvaluatingExpression (||) "DropView argument must be a symbol")
        (boss-eval (DropView 123)))

  (test "DropView with expression argument throws"
        '(ErrorWhenEvaluatingExpression (||) "DropView argument must be a symbol")
        (boss-eval (DropView (Table (A 1 2 3)))))

  (test "DropView on view currently being evaluated throws"
        '(ErrorWhenEvaluatingExpression (||) "Cannot drop view currently being evaluated: DROP5")
        (begin
          (boss-eval (DefineView DROP5 (Filter (DropView DROP5) (Greater A 1))))
          (boss-eval (QueryView DROP5))))
          
  (test "DropView succeeds after previously failing mid-evaluation"
        #t
        (begin
          (boss-eval (DefineView DROP6 (Filter (DropView DROP6) (Greater A 1))))
          (boss-eval (QueryView DROP6)) ;; throws, but stack must be cleaned up
          (boss-eval (DropView DROP6)))))

(test-group "ClearViews"

  (test "ClearViews returns true"
        #t
        (boss-eval (ClearViews)))

  (test "ClearViews with arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "ClearViews does not take any arguments")
        (boss-eval (ClearViews EXTRA)))

  (test "ClearViews removes all views"
        '(ErrorWhenEvaluatingExpression (||) "View not found: CLEAR1")
        (begin
          (boss-eval (DefineView CLEAR1 (Table (A 1 2 3))))
          (boss-eval (DefineView CLEAR2 (Table (B 4 5 6))))
          (boss-eval (ClearViews))
          (boss-eval (QueryView CLEAR1))))

  (test "ClearViews while view is being evaluated throws"
        '(ErrorWhenEvaluatingExpression (||) "Cannot clear views while 1 view(s) are being evaluated, e.g.: CLEAR3")
        (begin
          (boss-eval (DefineView CLEAR3 (Filter (ClearViews) (Greater A 1))))
          (boss-eval (QueryView CLEAR3))))

  (test "ClearViews is idempotent on empty registry"
        #t
        (begin
          (boss-eval (ClearViews))
          (boss-eval (ClearViews)))))

(test-group "ListViews"

  (test "ListViews with arguments throws"
        '(ErrorWhenEvaluatingExpression (||) "ListViews does not take any arguments")
        (boss-eval (ListViews EXTRA)))

  (test "ListViews on empty registry returns empty ViewList"
        '(ViewList (Name) (Definition))
        (begin
          (boss-eval (ClearViews))
          (boss-eval (ListViews))))

  (test "ListViews returns single view"
        '(ViewList (Name LIST1) (Definition (Table (A 1 2 3))))
        (begin
          (boss-eval (ClearViews))
          (boss-eval (DefineView LIST1 (Table (A 1 2 3))))
          (boss-eval (ListViews))))

  (test "ListViews returns multiple views"
        '(ViewList (Name LIST2 LIST3) (Definition (Table (A 1 2 3)) (Table (B 4 5 6))))
        (begin
          (boss-eval (ClearViews))
          (boss-eval (DefineView LIST2 (Table (A 1 2 3))))
          (boss-eval (DefineView LIST3 (Table (B 4 5 6))))
          (boss-eval (ListViews))))

  (test "ListViews reflects overwritten view"
        '(ViewList (Name LIST4) (Definition (Table (A 99))))
        (begin
          (boss-eval (ClearViews))
          (boss-eval (DefineView LIST4 (Table (A 1 2 3))))
          (boss-eval (DefineView LIST4 (Table (A 99))))
          (boss-eval (ListViews))))

  (test "ListViews does not show dropped view"
        '(ViewList (Name LIST6) (Definition (Table (B 4 5 6))))
        (begin
          (boss-eval (ClearViews))
          (boss-eval (DefineView LIST5 (Table (A 1 2 3))))
          (boss-eval (DefineView LIST6 (Table (B 4 5 6))))
          (boss-eval (DropView LIST5))
          (boss-eval (ListViews))))

  (test "ListViews returns unevaluated expressions"
        '(ViewList (Name LIST7) (Definition (QueryView LIST7)))
        (begin
          (boss-eval (ClearViews))
          (boss-eval (DefineView LIST7 (QueryView LIST7)))
          (boss-eval (ListViews)))))