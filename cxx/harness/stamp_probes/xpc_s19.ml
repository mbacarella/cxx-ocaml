module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
module type T = sig class b : object method a : int M.s -> int end end
module N : T = struct class b = object method a : int M.s -> int = fun x -> 0
  end end
