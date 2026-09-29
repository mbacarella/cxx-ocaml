module type T = sig type t = A end
module M : T = struct type t = A end
type u = { x : int }
