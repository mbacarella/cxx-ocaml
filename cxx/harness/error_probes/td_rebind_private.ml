type t = ..
module M : sig type t += private A end = struct type t += A end
type t += B = M.A
