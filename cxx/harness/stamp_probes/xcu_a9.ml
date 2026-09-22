type mine = A | B of { u : int }
module M = Buffer
include Stdlib.Fun
let c = M.create 8
