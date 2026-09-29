module rec A : sig type t val f : B.t -> int end
  = struct type t = int let f x = x end
and B : sig type t val g : B.t -> A.t end
  = struct type t = int let g x = x end
