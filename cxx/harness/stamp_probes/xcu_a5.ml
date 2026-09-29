module M = Map.Make (String)
module N = M
let z : int N.t = N.empty
type k = int M.t
