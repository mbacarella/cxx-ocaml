let f ?x () = match x with None -> 0 | Some v -> v
let a = f ()
let b = f ~x:3 ()
let c = f ?x:(Some 4) ()
let g ?(l = []) y = y :: l
