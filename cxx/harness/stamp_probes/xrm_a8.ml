module rec A : sig type t = X | Y exception E of t end
  = struct type t = X | Y exception E of t end
and B : sig type t = P | Q of A.t val g : t -> int end
  = struct type t = P | Q of A.t let g _ = 1 end
