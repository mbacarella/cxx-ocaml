module Id (X : sig type t end) = X
module G (X : sig type t end) = struct
  module P = struct type u = Id (X).t end
end
