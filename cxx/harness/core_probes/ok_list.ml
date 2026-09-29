let l = [1; 2; 3]
let l2 = 0 :: l
let s = List.fold_left ( + ) 0 l
let m = List.map string_of_int l
let h = match l with [] -> 0 | x :: _ -> x
