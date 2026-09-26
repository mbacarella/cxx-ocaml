module type T = sig type a val a : int end
module N = struct module type T = sig type b end end
module K : sig include T val z : int end = struct type a let a = 1 let z = 2 end
