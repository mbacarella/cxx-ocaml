module P = struct
  module type A = sig type t end
  module MyMap(X : A) = struct let module Z = X in () end
end
