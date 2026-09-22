module rec A : sig
  module N : sig type t = X | Y end
  type u = { a : N.t; b : B.t }
end = struct
  module N = struct type t = X | Y end
  type u = { a : N.t; b : B.t }
end
and B : sig type t = P | Q of A.u end
  = struct type t = P | Q of A.u end
