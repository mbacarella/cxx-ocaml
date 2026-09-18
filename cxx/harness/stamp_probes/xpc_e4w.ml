module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
module B = struct class type a = object method a : 'a. 'a M.s -> 'a end end
module M' = M module B' = B
class b = object method a : (module M.S with type a = int) -> int = fun x -> 0
  end
