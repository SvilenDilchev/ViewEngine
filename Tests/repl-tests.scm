(import (scheme base)
        (chibi test)
        (BOSS))

(boss-eval (SetDefaultEnginePipeline
    "/mnt/apps/lazy-loading/ViewEngine/build/libViewEngine.so"))

(test-group "ViewEngine"

  ;; --- DefineView ---

  (test "DefineView returns true on success"
        #t
        (boss-eval (DefineView MY_VIEW (Filter (Table (A 1 2 3)) (Greater A 1)))))

  (test "DefineView overwrites existing view"
        #t
        (begin
          (boss-eval (DefineView OVERWRITE (Table (A 1 2))))
          (boss-eval (DefineView OVERWRITE (Table (A 10 20))))))

  ;; --- DefineView failure cases (returns false) ---

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
        (boss-eval (DefineView (Table (A 1)) (Table (A 1 2 3)))))

  ;; --- QueryView ---

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
          (boss-eval (QueryView REPEATED))))

  ;; --- QueryView error cases ---

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
        (boss-eval (QueryView (Table (A 1 2 3)))))

  ;; --- Nested views ---

  (test "Nested view: QueryView inside stored expression gets resolved"
        '(Filter (Table (A 1 2 3)) (Greater A 1))
        (begin
          (boss-eval (DefineView INNER (Table (A 1 2 3))))
          (boss-eval (DefineView OUTER (Filter (QueryView INNER) (Greater A 1))))
          (boss-eval (QueryView OUTER))))

  (test "Nested view: three levels deep"
        '(GroupBy (Filter (Table (A 1 2 3)) (Greater A 1)) (Sum A))
        (begin
          (boss-eval (DefineView LEVEL1 (Table (A 1 2 3))))
          (boss-eval (DefineView LEVEL2 (Filter (QueryView LEVEL1) (Greater A 1))))
          (boss-eval (DefineView LEVEL3 (GroupBy (QueryView LEVEL2) (Sum A))))
          (boss-eval (QueryView LEVEL3))))

  ;; --- Nested DefineView (DefineView inside DefineView body) ---

  ;; INNER2 is stored unevaluated inside OUTER3's body — querying INNER2 before
  ;; OUTER3 throws because INNER2 has never been registered
  (test "Nested DefineView: querying inner before outer throws"
        '(ErrorWhenEvaluatingExpression (||) "View not found: INNER2")
        (begin
          (boss-eval (DefineView OUTER3
              (Filter (DefineView INNER2 (Table (A 1 2 3))) (Greater A 1))))
          (boss-eval (QueryView INNER2))))

  ;; Querying OUTER3 registers INNER2 as a side effect (returns bool in place of DefineView)
  ;; then querying INNER2 succeeds
  (test "Nested DefineView: querying outer registers inner as side effect"
        '(Table (A 1 2 3))
        (begin
          (boss-eval (DefineView OUTER4
              (Filter (DefineView INNER3 (Table (A 1 2 3))) (Greater A 1))))
          (boss-eval (QueryView OUTER4))
          (boss-eval (QueryView INNER3))))

  ;; --- QueryView inside expressions (non-top-level) ---

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
          (boss-eval (Project (QueryView BASE3) A))))

  ;; --- Circular view detection ---

  (test "Circular view: self-reference throws"
        '(ErrorWhenEvaluatingExpression (||) "Circular view dependency detected: SELF")
        (begin
          (boss-eval (DefineView SELF (QueryView SELF)))
          (boss-eval (QueryView SELF))))

  (test "Circular view: direct cycle A -> B -> A throws"
        '(ErrorWhenEvaluatingExpression (||) "Circular view dependency detected: CYCA")
        (begin
          (boss-eval (DefineView CYCA (QueryView CYCB)))
          (boss-eval (DefineView CYCB (QueryView CYCA)))
          (boss-eval (QueryView CYCA))))

  (test "Circular view: three-step cycle A -> B -> C -> A throws"
        '(ErrorWhenEvaluatingExpression (||) "Circular view dependency detected: TRIIA")
        (begin
          (boss-eval (DefineView TRIIA (QueryView TRIIB)))
          (boss-eval (DefineView TRIIB (QueryView TRIIC)))
          (boss-eval (DefineView TRIIC (QueryView TRIIA)))
          (boss-eval (QueryView TRIIA))))

  (test "Circular view: stack is clean after cycle, same view queryable again"
        '(Table (A 1 2 3))
        (begin
          (boss-eval (DefineView CLEAN_A (QueryView CLEAN_B)))
          (boss-eval (DefineView CLEAN_B (QueryView CLEAN_A)))
          (boss-eval (QueryView CLEAN_A))
          (boss-eval (DefineView CLEAN_A (Table (A 1 2 3))))
          (boss-eval (QueryView CLEAN_A))))

)