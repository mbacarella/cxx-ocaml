module M = struct type t = A end
open M
let x : t list = [A]
