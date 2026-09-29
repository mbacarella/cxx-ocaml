module rec A : sig type t val f : A.t -> int end
  = struct type t = int let f x = x end
