module rec A : sig type t = X | Y val f : t -> int end
  = struct type t = X | Y let f _ = 0 end
