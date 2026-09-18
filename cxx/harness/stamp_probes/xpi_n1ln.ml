module A = struct
 module type A_S = sig end
 type t = (module A_S)
end
module type S = sig type t end
module N = struct module type T = sig type t = (module A.A_S) end end
