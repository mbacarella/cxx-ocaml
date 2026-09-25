module M = struct type 'a e = R : 'a -> 'a e | S : int -> int e end
let a = M.R 1
let b = M.S 2
