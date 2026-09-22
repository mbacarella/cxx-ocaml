module type A = sig type u end
module type F = functor (X : A) -> sig type t val v : t end
module type G = functor (_ : A) -> sig type t end
module type H = sig type w end
