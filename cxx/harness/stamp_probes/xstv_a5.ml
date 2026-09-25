module M = struct type 'a e = R : 'a -> 'a e end
open M
let a = R 'c'
