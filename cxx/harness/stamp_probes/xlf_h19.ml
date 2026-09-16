module M : sig end = struct module Fast = struct module type S =
  sig type data end module type T = S end end
