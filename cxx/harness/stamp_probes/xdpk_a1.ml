module N = struct module type T = sig type b val w : int end end
let f (module X : N.T) = X.w
