(set-option :incremental false)
(set-logic QF_BV)
(declare-fun s () (_ BitVec 3))
(declare-fun t () (_ BitVec 3))

(assert (let ((_let_0 ((_ extract 2 2) s))) (let ((_let_1 ((_ extract 2 2) t))) (let ((_let_2 (= _let_0 (_ bv0 1)))) (let ((_let_3 (= _let_1 (_ bv0 1)))) (let ((_let_4 (bvneg s))) (let ((_let_5 (bvneg t))) (not (= (bvsdiv s t) (ite (and _let_2 _let_3) (bvudiv s t) (ite (and (= _let_0 (_ bv1 1)) _let_3) (bvneg (bvudiv _let_4 t)) (ite (and _let_2 (= _let_1 (_ bv1 1))) (bvneg (bvudiv s _let_5)) (bvudiv _let_4 _let_5)))))))))))))



