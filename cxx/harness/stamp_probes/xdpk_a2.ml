module N = struct module type T = sig type b val w : b end end
let f (module X : N.T with type b = int) = X.w
