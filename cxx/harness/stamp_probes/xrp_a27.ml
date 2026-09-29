module P = struct
  module type A = sig type t end
  module MyMap(X : A) = struct module Z = X module W = X end
end
