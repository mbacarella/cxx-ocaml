module M = struct
  module rec A : sig type t = X | Y of B.t end
    = struct type t = X | Y of B.t end
  and B : sig type t = P | Q of A.t end
    = struct type t = P | Q of A.t end
end
type u = U of M.A.t
