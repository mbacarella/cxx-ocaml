(* [%answer], [%double e], [%twice e] (one shared node), [%%item] *)
let a = [%answer]
let b = [%double 21]
let c = [%twice (a + 1)]
[%%item]
let d = generated + fst c
