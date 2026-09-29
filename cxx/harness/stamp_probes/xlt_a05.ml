module Id (X : sig type t end) = X
module G (X : sig type t end) = struct
  module Z = Id (X)
end
