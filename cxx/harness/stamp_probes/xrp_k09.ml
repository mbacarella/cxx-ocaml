module P = struct
  module type A = sig type t end
  module MyMap(X : A) = struct module Z = struct module W = X end end
end
