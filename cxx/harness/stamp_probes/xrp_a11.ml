module P = struct
  module MyMap(X : sig type t end) = X
end
