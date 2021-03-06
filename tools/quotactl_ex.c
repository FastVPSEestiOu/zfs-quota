
#include <sys/quota.h>
#include <stdio.h>
#include <fcntl.h>

int main()
{
	struct dqblk dqblk;
	quotactl(QCMD(Q_SYNC, 0), NULL, 0, NULL);
	quotactl(QCMD(Q_SYNC, 0), "simfs", 0, NULL);

	quotactl(QCMD(Q_GETQUOTA, USRQUOTA), "simfs", 0, (caddr_t) & dqblk);
	fprintf(stderr, "usage = %d\n", dqblk.dqb_curspace);
}
