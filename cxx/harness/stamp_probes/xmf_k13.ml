module type I = sig type t end
module Make(A : I)(B : I) : sig type t type 'a r = A : A.t r | B : B.t r
  end = struct type t = int type 'a r = A : A.t r | B : B.t r end
module X = Make(Int)(Int64)
