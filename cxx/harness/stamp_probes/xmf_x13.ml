module A = struct type t = int end
module B = struct type t = int end
let _ = Int.zero + Int64.to_int 0L
module F = Sys.Immediate64.Make
module X = Sys.Immediate64.Make(A)(B)
type u = X.t
