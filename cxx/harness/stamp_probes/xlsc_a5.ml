module N = struct module type T = sig type b val w : b end end
let f x = let ((module X) : (module N.T with type b = int)) = x in X.w
