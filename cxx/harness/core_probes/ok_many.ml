let a = List.init 3 (fun i -> i)
let b = List.combine a a
let c = List.split b
let d = List.assoc 1 b
let e = List.sort compare a
