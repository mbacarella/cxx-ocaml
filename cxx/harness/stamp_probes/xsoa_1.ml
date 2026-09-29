module M = struct type t = A end
open M
module type S = sig open Set.Make(Bool) type u = t end
let f (x : t) = x
