#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include <utils/hook.hpp>

namespace player_movement
{
	namespace
	{
		game::dvar_t* bg_omnimovement = nullptr;
		game::dvar_t* bg_sprintUnlimited = nullptr;
		game::dvar_t* bg_airControl = nullptr;

		utils::hook::detour pm_has_unlimited_sprint_hook;
		utils::hook::detour pm_slide_friction_hook;
		utils::hook::detour pm_slide_duration_hook;

		float pm_slide_friction_stub(void* pm, void* pml)
		{
			if (bg_omnimovement && bg_omnimovement->current.enabled)
			{
				return 0.0f;
			}

			return pm_slide_friction_hook.invoke<float>(pm, pml);
		}

		int pm_slide_duration_stub(void* ps, int energy)
		{
			const auto duration = pm_slide_duration_hook.invoke<int>(ps, energy);

			if (bg_omnimovement && bg_omnimovement->current.enabled)
			{
				return static_cast<int>(static_cast<float>(duration) * 1.25f);
			}

			return duration;
		}

		bool pm_has_unlimited_sprint_stub(void* ps)
		{
			if (bg_sprintUnlimited && bg_sprintUnlimited->current.enabled)
			{
				return true;
			}

			return pm_has_unlimited_sprint_hook.invoke<bool>(ps);
		}

		void jump_to(utils::hook::assembler& a, const std::uint64_t target)
		{
			a.sub(rsp, 8);
			a.push(rax);
			a.mov(rax, target);
			a.mov(qword_ptr(rsp, 8), rax);
			a.pop(rax);
			a.ret();
		}

		void* movement_scale_stub()
		{
			return utils::hook::assemble([](utils::hook::assembler& a)
			{
				const auto use_stock = a.newLabel();
				const auto omni = a.newLabel();
				const auto penalty = a.newLabel();

				a.movd(xmm0, ecx);
				a.cvtdq2ps(xmm0, xmm0);
				a.sqrtss(xmm3, xmm0);

				a.push(rax);
				a.mov(rax, reinterpret_cast<std::uint64_t>(&bg_omnimovement));
				a.mov(rax, qword_ptr(rax));
				a.test(rax, rax);
				a.jz(use_stock);
				a.mov(al, byte_ptr(rax, 0x10));
				a.test(al, al);
				a.jz(use_stock);
				a.pop(rax);
				a.jmp(omni);

				a.bind(use_stock);
				a.pop(rax);
				a.cmp(byte_ptr(r10, 0x128), 0);
				a.jz(penalty);

				a.bind(omni);
				jump_to(a, 0x1406F944F);

				a.bind(penalty);
				jump_to(a, 0x1406F945C);
			});
		}

		void* sprint_direction_stub()
		{
			return utils::hook::assemble([](utils::hook::assembler& a)
			{
				const auto use_stock = a.newLabel();
				const auto omni = a.newLabel();

				a.push(rcx);
				a.mov(rcx, reinterpret_cast<std::uint64_t>(&bg_omnimovement));
				a.mov(rcx, qword_ptr(rcx));
				a.test(rcx, rcx);
				a.jz(use_stock);
				a.mov(cl, byte_ptr(rcx, 0x10));
				a.test(cl, cl);
				a.jz(use_stock);
				a.pop(rcx);
				a.jmp(omni);

				a.bind(use_stock);
				a.pop(rcx);
				a.cmp(byte_ptr(rax, 0x128), 0);
				a.jnz(omni);

				jump_to(a, 0x1406F90DD);

				a.bind(omni);
				jump_to(a, 0x1406F90F1);
			});
		}

		void* sprint_engage_stub()
		{
			return utils::hook::assemble([](utils::hook::assembler& a)
			{
				const auto use_stock = a.newLabel();
				const auto omni = a.newLabel();

				a.push(rcx);
				a.mov(rcx, reinterpret_cast<std::uint64_t>(&bg_omnimovement));
				a.mov(rcx, qword_ptr(rcx));
				a.test(rcx, rcx);
				a.jz(use_stock);
				a.mov(cl, byte_ptr(rcx, 0x10));
				a.test(cl, cl);
				a.jz(use_stock);
				a.pop(rcx);
				a.jmp(omni);

				a.bind(use_stock);
				a.pop(rcx);
				a.cmp(byte_ptr(rax, 0x128), 0);
				a.jnz(omni);

				jump_to(a, 0x140700B81);

				a.bind(omni);
				jump_to(a, 0x140700B89);
			});
		}

		void* air_accel_stub()
		{
			return utils::hook::assemble([](utils::hook::assembler& a)
			{
				const auto use_stock = a.newLabel();
				const auto done = a.newLabel();

				a.push(rax);
				a.mov(rax, reinterpret_cast<std::uint64_t>(&bg_airControl));
				a.mov(rax, qword_ptr(rax));
				a.test(rax, rax);
				a.jz(use_stock);
				a.movss(xmm0, dword_ptr(rax, 0x10));
				a.pop(rax);
				a.jmp(done);

				a.bind(use_stock);
				a.mov(rax, 0x141438F0C);
				a.movss(xmm0, dword_ptr(rax));
				a.pop(rax);

				a.bind(done);
				jump_to(a, 0x1406F696A);
			});
		}

		void* slide_chain_stub()
		{
			return utils::hook::assemble([](utils::hook::assembler& a)
			{
				const auto skip = a.newLabel();

				a.push(rax);
				a.mov(rax, reinterpret_cast<std::uint64_t>(&bg_omnimovement));
				a.mov(rax, qword_ptr(rax));
				a.test(rax, rax);
				a.jz(skip);
				a.mov(al, byte_ptr(rax, 0x10));
				a.test(al, al);
				a.jz(skip);
				a.mov(rax, 0x141438F0C);
				a.movss(xmm6, dword_ptr(rax));

				a.bind(skip);
				a.pop(rax);
				a.mov(edx, ebp);
				a.mov(rcx, rbx);
				jump_to(a, 0x14070B67A);
			});
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			bg_omnimovement = game::Dvar_RegisterBool("bg_omnimovement", false, game::DVAR_FLAG_REPLICATED,
				"Sprint and slide in any direction");

			utils::hook::nop(0x1406F943A, 21);
			utils::hook::jump(0x1406F943A, movement_scale_stub());

			utils::hook::nop(0x1406F90D4, 9);
			utils::hook::jump(0x1406F90D4, sprint_direction_stub());

			utils::hook::nop(0x140700B78, 9);
			utils::hook::jump(0x140700B78, sprint_engage_stub());

			bg_sprintUnlimited = game::Dvar_RegisterBool("bg_sprintUnlimited", false, game::DVAR_FLAG_REPLICATED,
				"Remove the sprint duration limit");

			pm_has_unlimited_sprint_hook.create(0x1406FBC80, pm_has_unlimited_sprint_stub);

			bg_airControl = game::Dvar_RegisterFloat("bg_airControl", 1.0f, 0.0f, 100.0f,
				game::DVAR_FLAG_REPLICATED, "Air acceleration while off the ground");

			utils::hook::nop(0x1406F6962, 8);
			utils::hook::jump(0x1406F6962, air_accel_stub());

			utils::hook::nop(0x14070B675, 5);
			utils::hook::jump(0x14070B675, slide_chain_stub());

			pm_slide_friction_hook.create(0x14070A540, pm_slide_friction_stub);
			pm_slide_duration_hook.create(0x14070A870, pm_slide_duration_stub);
		}
	};
}

REGISTER_COMPONENT(player_movement::component)
