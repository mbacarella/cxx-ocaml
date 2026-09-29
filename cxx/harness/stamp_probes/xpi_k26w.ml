module A = struct
 module M = struct type t = int module N = struct type u = int end end
end
module type S = sig type t end
let _ = (module A.M : S with type t = int)
