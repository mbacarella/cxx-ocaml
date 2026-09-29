module rec A : sig type t = X | Y of B.t val f : t -> int end
  = struct type t = X | Y of B.t let f _ = 0 end
and B : sig type t = P | Q of A.t val g : t -> int end
  = struct type t = P | Q of A.t let g _ = 1 end
