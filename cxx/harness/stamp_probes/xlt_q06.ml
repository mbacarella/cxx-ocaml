module G (X : sig type t end) = struct
  module Y = X
end
