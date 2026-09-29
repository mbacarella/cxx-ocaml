module type I = sig type t end
module Make(A : I) : sig type t end = struct type t = int end
